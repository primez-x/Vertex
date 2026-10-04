#include "sketch/desktop/main_window.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QDir>
#include <algorithm>
#include <QFont>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace sketch;
using sketch::desktop::MainWindow;
using Json = nlohmann::json;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void prepare(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1280, 900); window.show();
    window.setMetricUnits(true); QApplication::processEvents();
}
Entity only_stroke(const DocumentSnapshot& snapshot) {
    Entity result; int count = 0;
    for (const auto& [id, entity] : snapshot.entities()) if (entity.type == "measurement_linework") { result = entity; ++count; }
    require(count == 1, "fixture has exactly one loose measured stroke"); return result;
}
Entity author_stroke(MainWindow& window) {
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0.01317, -0.01931}) &&
        window.appendMeasurementLineworkHeading("1.234567 m", "17.25 deg") &&
        window.appendMeasurementLineworkHeading("6 ft 6 3/4 in", "90 deg"), "native pen authors exact loose measured inputs");
    window.finishMeasurementLinework(); return only_stroke(window.document().snapshot());
}
void action(MainWindow& window, const char* name) {
    auto* command = window.findChild<QAction*>(QString::fromLatin1(name));
    require(command && command->isEnabled(), "native clipboard action is available"); command->trigger(); QApplication::processEvents();
}
void exact_history(MainWindow& window, const DocumentSnapshot& before, const DocumentSnapshot& after) {
    require(after.revision() == before.revision() + 1 && after.history().size() == before.history().size() + 1,
        "native clipboard edit is one history command");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == after.entities(), "one Undo and Redo restore all exact saved entities");
}
void persist(MainWindow& window, const QString& path) {
    const auto before = window.document().snapshot();
    require(window.saveProjectAs(path) && window.openProject(path) && window.document().snapshot().entities() == before.entities(),
        "native reopen retains exact loose strokes and dependent observations");
}
void capture(MainWindow& window, const char* filename) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "create isolated measured-line clipboard capture directory"); window.fitView();
    auto* tabs = window.findChild<QTabWidget*>("sidebarTabs"); require(tabs, "actual measured clipboard sidebar exists");
    for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == "Details") tabs->setCurrentIndex(i);
    QApplication::processEvents(); require(window.grab().save(QDir(directory).filePath(QString::fromLatin1(filename))),
        "capture actual measured-line clipboard canvas and Details");
}
void check_independent_stroke(const Entity& original, const Entity& copied);
void context_action(MainWindow& window, Vec2 point, const char* wanted, bool selected) {
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas, "native context canvas exists"); canvas->setOverviewMapEnabled(false);
    canvas->setFocus(); QApplication::processEvents();
    bool observed = false, complete = false;
    QTimer::singleShot(0, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        observed = true;
        const auto find = [&](const char* name) -> QAction* {
            for (auto* item : menu->actions()) if (item->objectName() == QString::fromLatin1(name)) return item;
            return nullptr;
        };
        complete = find("pasteSelection") && (!selected ||
            (find("copySelection") && find("cutSelection") && find("deleteSelection")));
        if (auto* item = find(wanted); complete && item) item->trigger();
        else complete = false;
        menu->close();
    });
    // Bound the native popup even when a future regression changes focus or
    // fails to expose the expected action; never leave an unattended menu open.
    QTimer::singleShot(1500, &window, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* menu = qobject_cast<QMenu*>(widget)) menu->close();
    });
    const auto view = canvas->viewCenter();
    const auto screen = QRectF(canvas->rect()).center() +
        QPointF((point.x - view.x) * canvas->viewScale(), -(point.y - view.y) * canvas->viewScale());
    QMouseEvent press(QEvent::MouseButtonPress, screen, canvas->mapToGlobal(screen.toPoint()),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, screen, canvas->mapToGlobal(screen.toPoint()),
        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release); QApplication::processEvents();
    require(observed && complete, "real canvas context exposes and executes supported clipboard actions");
}
void native_canvas_context_clipboard() {
    QTemporaryDir directory;
    MainWindow source({}, nullptr, directory.filePath("context-source.json")); prepare(source);
    const auto original = author_stroke(source); source.fitView();
    require(source.selectEntity(QString::fromStdString(original.id)), "context source is selected");
    const auto edge = replay_measurement_linework(*decode_measurement_linework_model(original.properties.at("model")).model).edges.front().segment;
    const Vec2 midpoint{(edge.start.x + edge.end.x) / 2, (edge.start.y + edge.end.y) / 2};
    context_action(source, midpoint, "cutSelection", true);
    require(!source.document().snapshot().entities().contains(original.id), "right-click Cut removes the selected measured stroke");
    const auto cut_state = source.document().snapshot();
    context_action(source, {}, "pasteSelection", false);
    check_independent_stroke(original, only_stroke(source.document().snapshot()));
    exact_history(source, cut_state, source.document().snapshot());
    MainWindow target({}, nullptr, directory.filePath("context-target.json")); prepare(target);
    context_action(target, {}, "pasteSelection", false);
    check_independent_stroke(original, only_stroke(target.document().snapshot()));
    capture(target, "linework-context-pasted.png");
}
Json square(double x, double y, double width, double height) {
    return Json::array({{{"start", {x, y}}, {"end", {x + width, y}}, {"sweep_radians", 0}},
        {{"start", {x + width, y}}, {"end", {x + width, y + height}}, {"sweep_radians", 0}},
        {{"start", {x + width, y + height}}, {"end", {x, y + height}}, {"sweep_radians", 0}},
        {{"start", {x, y + height}}, {"end", {x, y}}, {"sweep_radians", 0}}});
}
Entity decorate_and_edit(MainWindow& window) {
    auto original = author_stroke(window); auto decoded = decode_measurement_linework_model(original.properties.at("model"));
    decoded.model->extensions["vendor"] = {{"literal", decoded.model->edges.front().segment_id}, {"settings", {7, "retain raw"}}};
    original.properties["model"] = encode_measurement_linework_model(*decoded.model);
    original.extensions["vendor"] = {{"notes", "unmodified opaque entity metadata"}};
    auto other = original; other.id = "unrelated-area"; other.type = "boundary"; other.required = false; other.extensions = Json::object();
    other.properties.erase("model"); other.properties.update({{"boundary", square(10, 10, 1, 1)}, {"classification", "living"}, {"factor", 1}});
    other = upgrade_legacy_boundary_entity(other);
    AnnotationState state;
    PresentationOverride owned; owned.target_kind = "area"; owned.target_id = original.id;
    owned.style.stroke_color = "#315a8c"; owned.style.stroke_width_metres = 0.0037; owned.paper_line_width_mm = 0.42;
    state.overrides.push_back(owned); auto unrelated = owned; unrelated.target_id = other.id; unrelated.style.stroke_color = "#853b27";
    state.overrides.push_back(unrelated);
    window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(original), EntityChange::upsert(other),
        EntityChange::upsert(make_annotation_entity("stroke-appearance", state))}, {}, "Typed clipboard metadata fixture"});
    const auto replayed = replay_measurement_linework(*decoded.model); auto endpoint = replayed.edges.front().segment.end; endpoint.y += 0.1234;
    require(window.selectEntity(QString::fromStdString(original.id)) && window.moveSelectedBoundaryVertex(
        QString::fromStdString(decoded.model->edges.front().end_vertex_id), endpoint, window.document().revision()),
        "native loose vertex edit authors a retained operation over original typed receipts");
    return only_stroke(window.document().snapshot());
}
void check_independent_stroke(const Entity& original, const Entity& copied) {
    const auto old = decode_measurement_linework_model(original.properties.at("model"));
    const auto next = decode_measurement_linework_model(copied.properties.at("model"));
    require(old.supported() && next.supported() && copied.required && copied.id != original.id &&
        next.model->stroke_id == copied.id && old.model->edges.size() == next.model->edges.size(), "copied stroke is independently identified required geometry");
    require(copied.extensions == original.extensions && next.model->extensions == old.model->extensions,
        "entity and model opaque metadata remain exact even when containing identity-looking literals");
    std::map<std::string, std::string> identities{{original.id, copied.id}}; std::set<std::string> old_ids{original.id};
    for (const auto& edge : old.model->edges) {
        old_ids.insert(edge.segment_id); old_ids.insert(edge.start_vertex_id); old_ids.insert(edge.end_vertex_id);
    }
    for (std::size_t i = 0; i < old.model->edges.size(); ++i) {
        const auto& source = old.model->edges[i]; const auto& edge = next.model->edges[i];
        require(!old_ids.contains(edge.segment_id) && !old_ids.contains(edge.start_vertex_id) && !old_ids.contains(edge.end_vertex_id),
            "all copied measured segment and vertex identities are fresh");
        identities[source.segment_id] = edge.segment_id; identities[source.start_vertex_id] = edge.start_vertex_id; identities[source.end_vertex_id] = edge.end_vertex_id;
        auto expected = encode_construction_receipt(source.receipt); expected["segment_id"] = edge.segment_id;
        require(encode_construction_receipt(edge.receipt) == expected, "copy retains every raw typed receipt except its fresh identity");
    }
    require(next.model->operations.size() >= old.model->operations.size(), "copy retains ordered native edit history");
    for (std::size_t i = 0; i < old.model->operations.size(); ++i) if (const auto* edit = std::get_if<MeasurementLineworkEdit>(&old.model->operations[i])) {
        const auto* copied_edit = std::get_if<MeasurementLineworkEdit>(&next.model->operations[i]);
        auto expected = edit->intent; expected.boundary_id = identities.at(expected.boundary_id); expected.target_id = identities.at(expected.target_id);
        require(copied_edit && copied_edit->intent == expected && copied_edit->authored_length.has_value() == edit->authored_length.has_value(),
            "copied receipt operations bind to their fresh stroke and stable child identities");
        if (edit->authored_length) require(copied_edit->authored_length->metres == edit->authored_length->metres &&
            copied_edit->authored_length->original_expression == edit->authored_length->original_expression, "copied edit retains exact authored length");
    }
    const auto before = replay_measurement_linework(*old.model), after = replay_measurement_linework(*next.model);
    const Vec2 delta{after.edges.front().segment.start.x - before.edges.front().segment.start.x,
        after.edges.front().segment.start.y - before.edges.front().segment.start.y};
    for (std::size_t i = 0; i < before.edges.size(); ++i) {
        const auto& a = before.edges[i].segment; const auto& b = after.edges[i].segment;
        require(std::abs(b.start.x - a.start.x - delta.x) < 1e-8 && std::abs(b.start.y - a.start.y - delta.y) < 1e-8 &&
            std::abs(b.end.x - a.end.x - delta.x) < 1e-8 && std::abs(b.end.y - a.end.y - delta.y) < 1e-8 && a.sweep_radians == b.sweep_radians,
            "copied derived geometry retains its edited analytical shape under clipboard placement");
    }
}
void check_appearance(MainWindow& target, const Entity& original, const Entity& copied, const Entity& source_style) {
    bool retained = false; const auto snapshot = target.document().snapshot();
    for (const auto& [id, entity] : snapshot.entities()) if (entity.type == kAnnotationEntityType)
        for (const auto& record : entity.properties.at("state").at("overrides")) if (record.at("target_id") == copied.id) {
            auto expected = source_style.properties.at("state").at("overrides").at(0); expected["target_id"] = copied.id;
            require(record == expected, "pure-stroke copy retains exact owned appearance with fresh target"); retained = true;
        }
    require(retained, "copied loose stroke includes its actual owned appearance");
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(target.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas, "copied stroke projects to native canvas"); bool drawn = false;
    for (const auto& item : canvas->entities()) if (item.id.toStdString() == copied.id) {
        drawn = true; require(item.stroke_color == QColor("#315a8c") && std::abs(item.output_stroke_width_mm - 0.42) < 1e-12,
            "actual copied stroke renderer retains color and paper line width");
    }
    require(drawn, "copied loose stroke is actual selectable native geometry"); (void)original;
}
void native_copy_to_empty_project() {
    QTemporaryDir directory; require(directory.isValid(), "isolated clipboard fixture");
    MainWindow source({}, nullptr, directory.filePath("source-library.json")); prepare(source);
    const auto original = author_stroke(source); const auto before = source.document().snapshot();
    require(source.selectEntity(QString::fromStdString(original.id)), "select actual authored measured stroke");
    if (!source.copySelection()) throw std::runtime_error("Native loose measured-line Copy must succeed: " + source.lastError().toStdString());
    require(source.document().snapshot().entities() == before.entities() && source.document().revision() == before.revision(),
        "Copy preserves exact source document");
    MainWindow target({}, nullptr, directory.filePath("target-library.json")); prepare(target);
    const auto empty = target.document().snapshot();
    if (!target.pasteSelection()) throw std::runtime_error("Native measured-line Paste into a fresh empty project must succeed: " + target.lastError().toStdString());
    const auto pasted = target.document().snapshot(); const auto copied = only_stroke(pasted);
    require(copied.id != original.id && copied.required && pasted.revision() == empty.revision() + 1,
        "fresh empty project receives one independent required measured stroke atomically");
    const auto old_model = decode_measurement_linework_model(original.properties.at("model"));
    const auto new_model = decode_measurement_linework_model(copied.properties.at("model"));
    require(old_model.supported() && new_model.supported() && new_model.model->edges.size() == 2,
        "pasted loose stroke retains its exact complete typed model");
    for (std::size_t i = 0; i < old_model.model->edges.size(); ++i) {
        const auto& old = old_model.model->edges[i]; const auto& edge = new_model.model->edges[i];
        require(edge.segment_id != old.segment_id && edge.start_vertex_id != old.start_vertex_id && edge.end_vertex_id != old.end_vertex_id,
            "pasted measured edges and vertices receive fresh identities");
        require(edge.receipt.distance->original_expression == old.receipt.distance->original_expression &&
            edge.receipt.heading->original_expression == old.receipt.heading->original_expression,
            "pasted typed distance and heading expressions remain exact");
    }
    require(target.undoCommand() && target.document().snapshot().entities() == empty.entities() &&
        target.redoCommand() && target.document().snapshot().entities() == pasted.entities(), "paste is one exact Undo and Redo step");
    const auto saved = target.document().snapshot(); const auto project = directory.filePath("loose-copy.bldproj");
    require(target.saveProjectAs(project) && target.openProject(project) && target.document().snapshot().entities() == saved.entities(),
        "native save and reopen retain copied measured identities and raw inputs exactly");
}
void native_actions_and_owned_appearance() {
    QTemporaryDir directory; require(directory.isValid(), "isolated native action fixture");
    for (const bool cut : {false, true}) {
        MainWindow source({}, nullptr, directory.filePath(cut ? "cut-source.json" : "copy-source.json")); prepare(source);
        const auto original = decorate_and_edit(source); const auto before = source.document().snapshot();
        require(source.selectEntity(QString::fromStdString(original.id)), "select actual styled loose stroke");
        action(source, cut ? "cutSelection" : "copySelection"); const auto after = source.document().snapshot();
        if (cut) {
            require(!after.entities().contains(original.id) && after.entities().at("unrelated-area") == before.entities().at("unrelated-area"),
                "actual Cut removes only selected loose geometry and preserves unrelated areas");
            const auto& saved = after.entities().at("stroke-appearance").properties.at("state").at("overrides");
            const auto& unrelated = before.entities().at("stroke-appearance").properties.at("state").at("overrides").at(1);
            require(std::find(saved.begin(), saved.end(), unrelated) != saved.end(), "Cut retains unselected appearance record exactly");
            exact_history(source, before, after);
        } else require(after.entities() == before.entities() && after.revision() == before.revision(), "native Copy is read only");
        const auto payload = Json::parse(QApplication::clipboard()->text().toStdString()); int owned_overrides = 0;
        for (const auto& entity : payload.at("entities")) if (entity.at("type") == kAnnotationEntityType)
            owned_overrides += static_cast<int>(entity.at("properties").at("state").at("overrides").size());
        require(owned_overrides == 1, "pure-stroke clipboard includes only the selected stroke's owned appearance");
        MainWindow target({}, nullptr, directory.filePath(cut ? "cut-target.json" : "copy-target.json")); prepare(target);
        const auto empty = target.document().snapshot(); action(target, "pasteSelection"); const auto pasted = target.document().snapshot();
        const auto copied = only_stroke(pasted); check_independent_stroke(original, copied);
        check_appearance(target, original, copied, before.entities().at("stroke-appearance"));
        exact_history(target, empty, pasted); capture(target, cut ? "linework-cut-pasted.png" : "linework-copy-pasted.png");
        persist(target, directory.filePath(cut ? "native-cut.bldproj" : "native-copy.bldproj"));
    }
    MainWindow source({}, nullptr, directory.filePath("mixed-source.json")); prepare(source);
    const auto original = decorate_and_edit(source); const auto before = source.document().snapshot();
    require(source.selectEntity(QString::fromStdString(original.id)) && source.selectEntity("unrelated-area", true), "select measured stroke and independent area together");
    action(source, "copySelection"); const auto valid_clipboard = QApplication::clipboard()->text();
    MainWindow target({}, nullptr, directory.filePath("mixed-target.json")); prepare(target); const auto empty = target.document().snapshot();
    action(target, "pasteSelection"); const auto pasted = target.document().snapshot();
    check_independent_stroke(original, only_stroke(pasted)); int boundaries = 0, overrides = 0;
    for (const auto& [id, entity] : pasted.entities()) {
        if (entity.type == "boundary") ++boundaries;
        if (entity.type == kAnnotationEntityType) overrides += static_cast<int>(entity.properties.at("state").at("overrides").size());
    }
    require(boundaries == 1 && overrides == 2 && source.document().snapshot().entities() == before.entities(),
        "mixed area and stroke copy retains both owned appearances without altering originals"); exact_history(target, empty, pasted);
    // A foreign target is real destination geometry, so accepting this record would alter an unselected object's appearance.
    const auto foreign = target.createBoundary({{{20, 20}, {21, 20}, 0}, {{21, 20}, {21, 21}, 0},
        {{21, 21}, {20, 21}, 0}, {{20, 21}, {20, 20}, 0}}, "living"); require(!foreign.isEmpty(), "foreign destination target is real geometry");
    auto malicious = Json::parse(valid_clipboard.toStdString()); bool injected = false;
    for (auto& entity : malicious["entities"]) if (entity.at("type") == kAnnotationEntityType) {
        entity["properties"]["state"]["overrides"][0]["target_id"] = foreign.toStdString(); injected = true;
    }
    require(injected, "foreign override injection uses actual native payload"); const auto encoded = QString::fromStdString(malicious.dump());
    QApplication::clipboard()->setText(encoded); const auto retained = target.document().snapshot();
    require(!target.pasteSelection() && target.document().snapshot().entities() == retained.entities() && target.document().revision() == retained.revision() &&
        target.document().snapshot().history().size() == retained.history().size() && QApplication::clipboard()->text() == encoded,
        "foreign-target clipboard appearance refuses atomically instead of styling existing destination geometry");
}
Entity source_stroke(const std::string& id, const std::vector<Vec2>& points, bool closed) {
    MeasurementLinework model; model.stroke_id = id; model.anchor = points.front(); model.closed = closed;
    for (std::size_t i = 1; i < points.size(); ++i) {
        ConstructionReceipt receipt; receipt.segment_id = id + ":edge" + std::to_string(i);
        receipt.kind = BoundaryConstructionKind::line_to_point; receipt.start = points[i - 1]; receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, id + ":vertex" + std::to_string(i - 1),
            closed && i + 1 == points.size() ? id + ":vertex0" : id + ":vertex" + std::to_string(i), receipt});
    }
    return {id, "measurement_linework", {{"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"}, {"layer_id", "l"},
        {"model", encode_measurement_linework_model(model)}}, true};
}
Json source_use(const char* owner, int edge, double start, double end, bool reversed = false) {
    return Json::array({{{"owner_id", owner}, {"segment_id", std::string(owner) + ":edge" + std::to_string(edge)},
        {"parameter_start", start}, {"parameter_end", end}, {"reversed", reversed}}});
}
std::shared_ptr<Document> shared_source_fixture() {
    const Json facts{{"finish", "finished"}, {"access", "direct_interior"}, {"ceiling_eligibility", "standard"},
        {"area_use", "dwelling"}, {"boundary_role", "measured_area"}};
    auto left = upgrade_legacy_boundary_entity({"left-area", "measurement_boundary", {{"property_id", "p"}, {"building_id", "b"},
        {"floor_id", "f"}, {"layer_id", "l"}, {"boundary", square(0, 0, 2, 4)}, {"classification", "living"}, {"factor", 1},
        {"appraisal_facts", facts}}, false});
    auto right = upgrade_legacy_boundary_entity({"right-area", "measurement_boundary", {{"property_id", "p"}, {"building_id", "b"},
        {"floor_id", "f"}, {"layer_id", "l"}, {"boundary", square(2, 0, 2, 4)}, {"classification", "living"}, {"factor", 1},
        {"appraisal_facts", facts}}, false});
    left.extensions["measurement_linework_sources"] = Json::array({source_use("outline", 1, 0, .5),
        source_use("separator", 1, 1.0 / 6, 5.0 / 6), source_use("outline", 3, .5, 1), source_use("outline", 4, 0, 1)});
    right.extensions["measurement_linework_sources"] = Json::array({source_use("outline", 1, .5, 1), source_use("outline", 2, 0, 1),
        source_use("outline", 3, 0, .5), source_use("separator", 1, 1.0 / 6, 5.0 / 6, true)});
    return std::make_shared<Document>(Document::create({
        {"p", "property", {{"calculation_workflow", "appraisal"}, {"appraisal_policy", {{"policy_kind", "residential_declared"},
            {"version", 1}, {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"}}}}, false},
        {"b", "building", {{"property_id", "p"}}, false}, {"f", "floor", {{"building_id", "b"}, {"appraisal_facts", {{"grade", "above"}}}}, false},
        {"l", "layer", {{"floor_id", "f"}}, false}, source_stroke("outline", {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}, true),
        source_stroke("separator", {{2, -1}, {2, 5}}, false), source_stroke("unrelated-source", {{10, 10}, {11, 10}}, false), left, right}));
}
void check_shared_sources(const DocumentSnapshot& snapshot, bool current) {
    const auto checks = measurement_linework_source_checks(snapshot.entities());
    require(checks.at("left-area").current == current && checks.at("right-area").current == current,
        "both real consumers share the source freshness state");
    const auto report = build_appraisal_document_report(snapshot, "p", AreaUnit::square_metre);
    if (current) require(report.qualified && report.calculation && std::abs(report.calculation->property.gla().total.square_metres - 16) < 1e-8,
        "two current disjoint measured faces contribute exact GLA16");
    else {
        require(!report.qualified && !report.calculation, "deleted shared source withholds GLA without inventing replacement observations");
        for (const auto& status : report.boundaries) require(!status.measurement && status.facts.has_value(),
            "stale derived areas retain facts while exposing no outdated measurement");
    }
}
void delete_shared_source_preserves_areas() {
    QTemporaryDir directory; MainWindow window(shared_source_fixture(), nullptr, directory.filePath("delete-library.json")); prepare(window);
    const auto before = window.document().snapshot(); check_shared_sources(before, true);
    require(window.selectEntity("separator"), "select actual shared source stroke for Delete"); action(window, "deleteSelection");
    const auto deleted = window.document().snapshot(); require(!deleted.entities().contains("separator"), "native Delete removes selected required measured source");
    for (const auto* id : {"left-area", "right-area", "outline", "unrelated-source"})
        require(deleted.entities().at(id) == before.entities().at(id), "Delete retains independent derived boundaries facts lineage and unselected sources exactly");
    check_shared_sources(deleted, false); exact_history(window, before, deleted);
    require(window.selectEntity("left-area"), "select retained stale measured area for actual Details capture");
    capture(window, "linework-shared-source-deleted.png");
    persist(window, directory.filePath("deleted-shared-source.bldproj")); check_shared_sources(window.document().snapshot(), false);
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(), "native Undo after reopen restores shared source and all observations exactly");
    check_shared_sources(window.document().snapshot(), true);
}
void unsafe_models_and_payloads_are_atomic() {
    QTemporaryDir directory; MainWindow author({}, nullptr, directory.filePath("safe-source.json")); prepare(author);
    const auto original = author_stroke(author); require(author.selectEntity(QString::fromStdString(original.id)) && author.copySelection(), "capture real supported loose-stroke payload");
    const auto valid_clipboard = QApplication::clipboard()->text(); const auto snapshot = author.document().snapshot();
    for (const bool future : {false, true}) {
        auto document = std::make_shared<Document>(Document::fork(snapshot));
        if (future) {
            auto opaque = original; opaque.properties["model"]["version"] = 999;
            document->apply(ApplyEntityChanges{document->revision(), {EntityChange::upsert(opaque)}, {}, "Opaque future model fixture"});
        } else document->mark_read_only("clipboard read-only fixture");
        MainWindow refused(document, nullptr, directory.filePath(future ? "future-library.json" : "readonly-library.json")); prepare(refused);
        (void)refused.selectEntity(QString::fromStdString(original.id)); const auto before = refused.document().snapshot();
        require(!refused.cutSelection() && !refused.deleteSelection() && !refused.pasteSelection(), "read-only or future-model project refuses native document mutations");
        require(refused.document().snapshot().entities() == before.entities() && refused.document().revision() == before.revision() &&
            refused.document().snapshot().history().size() == before.history().size() && QApplication::clipboard()->text() == valid_clipboard,
            "rejected Cut Delete and Paste preserve exact document history and clipboard");
    }
    for (const bool populated : {false, true}) {
        MainWindow target({}, nullptr, directory.filePath(populated ? "populated-library.json" : "empty-library.json")); prepare(target);
        if (populated) require(!target.createBoundary({{{0, 0}, {1, 0}, 0}, {{1, 0}, {1, 1}, 0}, {{1, 1}, {0, 1}, 0}, {{0, 1}, {0, 0}, 0}}, "living").isEmpty(),
            "populated target owns real independent geometry");
        for (const bool unsupported_owner : {false, true}) {
            auto payload = Json::parse(valid_clipboard.toStdString());
            if (unsupported_owner) payload["entities"].push_back({{"id", "forged-property"}, {"type", "property"}, {"required", false},
                {"properties", {{"name", "Not clipboard geometry"}}}, {"extensions", Json::object()}});
            else for (auto& entity : payload["entities"]) if (entity.at("type") == "measurement_linework") entity["properties"]["model"]["version"] = 999;
            const auto encoded = QString::fromStdString(payload.dump()); QApplication::clipboard()->setText(encoded); const auto before = target.document().snapshot();
            require(!target.pasteSelection() && target.document().snapshot().entities() == before.entities() && target.document().revision() == before.revision() &&
                target.document().snapshot().history().size() == before.history().size() && QApplication::clipboard()->text() == encoded,
                "future stroke or unsupported extra owner refuses atomically in empty and populated destinations");
        }
    }
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-linework-clipboard-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0, "bundled Inter font loads");
        application.setFont(QFont(QStringLiteral("Inter"), 10)); native_copy_to_empty_project();
        native_canvas_context_clipboard();
        native_actions_and_owned_appearance(); delete_shared_source_preserves_areas(); unsafe_models_and_payloads_are_atomic();
    } catch (const std::exception& error) { std::cerr << "measurement_linework_clipboard_desktop_tests: " << error.what() << '\n'; return 1; }
    std::cout << "Measured-line clipboard desktop tests passed\n"; return 0;
}
