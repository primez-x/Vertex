#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
using desktop::MainWindow;
using K = RoomReferenceKind;
using R = RoomRelationKind;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef near
#undef near
#endif
bool near(double left, double right) { return std::abs(left - right) < 1e-9; }

template<class Widget>
Widget& child(QWidget& owner, const char* name) {
    auto* control = dynamic_cast<Widget*>(owner.findChild<QWidget*>(QString::fromLatin1(name)));
    require(control != nullptr, "actual relationship dialog must expose its named control");
    return *control;
}

void choose(QComboBox& control, const std::string& id) {
    const auto index = control.findData(QString::fromStdString(id));
    require(index >= 0, "propagation dialog must offer the declared driver identity");
    control.setCurrentIndex(index);
}

void capture(QWidget& widget, const char* name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QApplication::processEvents();
    require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name)),
        "actual relationship preview capture must save");
}

// Failures in Qt callbacks must unwind both nested modal loops, then reach main.
void propagation_dialog(MainWindow& window, const std::function<void(QDialog&)>& interact) {
    std::exception_ptr failure;
    bool relationship_opened = false;
    bool preview_opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* relationships = window.findChild<QDialog*>("roomRelationshipsDialog");
        try {
            require(relationships != nullptr, "Room relationships must open its actual editor");
            relationship_opened = true;
            auto& preview_button = child<QPushButton>(*relationships, "previewRoomRelationshipPropagation");
            QTimer::singleShot(0, &window, [&] {
                auto* preview = window.findChild<QDialog*>("roomRelationshipPropagationDialog");
                try {
                    require(preview != nullptr, "Preview propagation must open its actual modal dialog");
                    preview_opened = true;
                    interact(*preview);
                } catch (...) { failure = std::current_exception(); }
                if (preview && preview->isVisible()) preview->reject();
            });
            preview_button.click();
            // Drain the scheduled interaction even if opening the inner dialog
            // refused before exec(), while this callback's locals still live.
            QApplication::processEvents();
        } catch (...) { failure = std::current_exception(); }
        if (relationships && relationships->isVisible()) relationships->reject();
    });
    window.showRoomRelationships();
    if (failure) std::rethrow_exception(failure);
    require(relationship_opened && preview_opened, "both actual relationship dialogs must execute");
}

Entity receipt_rectangle(std::string id, std::string type, double left) {
    const Vec2 points[]{{left, 0}, {left + 4, 0}, {left + 4, 3}, {left, 3}};
    const char* rises[]{"0 m", "3 m", "0 m", "-3 m"};
    const char* runs[]{"4 m", "0 m", "-4 m", "0 m"};
    IdentifiedBoundary boundary{id, std::move(type), {}};
    BoundaryConstructionRecord record;
    record.boundary_id = id;
    record.anchor = points[0];
    for (std::size_t index = 0; index < 4; ++index) {
        const auto edge = id + "-edge-" + std::to_string(index);
        const auto start = id + "-vertex-" + std::to_string(index);
        const auto end = id + "-vertex-" + std::to_string((index + 1) % 4);
        ConstructionReceipt receipt;
        receipt.segment_id = edge;
        receipt.kind = BoundaryConstructionKind::line_rise_run;
        receipt.start = points[index];
        receipt.rise = parse_quantity(rises[index]);
        receipt.run = parse_quantity(runs[index]);
        record.edges.push_back({edge, start, end, receipt});
        boundary.segments.push_back({edge, start, end, {points[index], points[(index + 1) % 4], 0}});
    }
    auto entity = encode_identified_boundary_entity(boundary);
    entity.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    entity.properties["fixture_metadata"] = {{"note", "retain this annotation"}};
    entity.extensions["fixture_extension"] = id;
    return entity;
}

Entity relation_entity(std::vector<RoomReference> references, std::vector<RoomRelation> relations) {
    return {"relationships", "room_relationships",
        {{"model", RoomRelationshipSnapshot::create(std::move(references), std::move(relations)).to_json()}},
        false, nlohmann::json::object()};
}

std::shared_ptr<Document> area_document(bool conflicting = false) {
    auto driver = receipt_rectangle("driver", "room_boundary", 0);
    auto dependent = receipt_rectangle("dependent", "measurement_boundary", 8);
    std::vector<RoomReference> references{{"driver", K::room_boundary},
                                        {"dependent", K::appraisal_measurement_boundary}};
    std::vector<RoomRelation> relations{{"dependent", "driver", conflicting ? R::derived_from : R::follows}};
    std::vector<Entity> entities{driver, dependent,
        encode_boundary_dimension_entity({"driver-dimension", "driver", "driver-edge-0", {2, -1}}),
        encode_boundary_dimension_entity({"dependent-dimension", "dependent", "dependent-edge-0", {10, -1}})};
    if (conflicting) {
        entities.push_back(receipt_rectangle("other-driver", "room_boundary", 16));
        references.push_back({"other-driver", K::room_boundary});
        relations.push_back({"dependent", "other-driver", R::derived_from});
    }
    entities.push_back(relation_entity(std::move(references), std::move(relations)));
    return std::make_shared<Document>(Document::create(std::move(entities)));
}

void prepare_window(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    window.resize(1100, 780);
    window.show();
    QApplication::processEvents();
}

void set_move(QDialog& dialog, const std::string& driver, const char* dx = "2.5 m", const char* dy = "1.25 m") {
    choose(child<QComboBox>(dialog, "roomRelationshipPropagationDriver"), driver);
    child<QLineEdit>(dialog, "roomRelationshipPropagationOffsetX").setText(dx);
    child<QLineEdit>(dialog, "roomRelationshipPropagationOffsetY").setText(dy);
}

QPushButton& apply_button(QDialog& dialog) {
    auto* button = child<QDialogButtonBox>(dialog, "roomRelationshipPropagationButtons")
        .button(QDialogButtonBox::Apply);
    require(button != nullptr, "relationship dialog must expose its Apply button");
    return *button;
}

void require_preview_start(QDialog& dialog, const std::string& owner, double x, double y) {
    const auto& entities = child<desktop::PlanCanvas>(dialog, "roomRelationshipPropagationPreview").entities();
    const auto found = std::find_if(entities.begin(), entities.end(), [&](const auto& entity) {
        return entity.selected && entity.id.startsWith(QString::fromStdString(owner) + QStringLiteral(" · "));
    });
    require(found != entities.end() && !found->segments.empty() &&
            near(found->segments.front().start.x, x) && near(found->segments.front().start.y, y),
        "actual proposed preview must contain the driver and dependent at their requested positions");
}

void require_translation(const Entity& before, const Entity& after, double dx, double dy) {
    const auto original = decode_identified_boundary_entity(before);
    const auto moved = decode_identified_boundary_entity(after);
    require(original.id == moved.id && original.type == moved.type &&
            original.segments.size() == moved.segments.size(), "move must retain owner identity and topology");
    for (std::size_t index = 0; index < original.segments.size(); ++index) {
        const auto& first = original.segments[index];
        const auto& second = moved.segments[index];
        require(first.segment_id == second.segment_id && first.start_vertex_id == second.start_vertex_id &&
                first.end_vertex_id == second.end_vertex_id && first.segment.sweep_radians == second.segment.sweep_radians &&
                near(second.segment.start.x, first.segment.start.x + dx) &&
                near(second.segment.start.y, first.segment.start.y + dy) &&
                near(second.segment.end.x, first.segment.end.x + dx) &&
                near(second.segment.end.y, first.segment.end.y + dy),
            "Apply must move BOTH the driver and dependent while retaining all stable children");
    }
    require(before.properties.at("fixture_metadata") == after.properties.at("fixture_metadata") &&
            before.extensions == after.extensions, "move must retain unrelated annotations and extensions");
    const auto original_receipt = decode_boundary_receipt_envelope(before.properties.at("boundary_authoring"));
    const auto moved_receipt = decode_boundary_receipt_envelope(after.properties.at("boundary_authoring"));
    require(original_receipt.supported() && moved_receipt.supported(), "both moved owners must retain supported authoring evidence");
    require(moved_receipt.record->edges == original_receipt.record->edges &&
            moved_receipt.record->transforms.size() == original_receipt.record->transforms.size() + 1,
        "rigid movement must retain exact construction receipts and append one transform");
}

void test_area_move_and_history() {
    QTemporaryDir directory;
    require(directory.isValid(), "relationship move needs a temporary project directory");
    MainWindow window(area_document(), nullptr, directory.filePath("library.json"));
    prepare_window(window);
    require(window.selectEntity("driver"), "select the reference driver in the actual workspace");
    const auto before = window.document().snapshot();
    propagation_dialog(window, [&](QDialog& dialog) {
        set_move(dialog, "driver");
        require(apply_button(dialog).isEnabled(), "valid driver/dependent move must enable Apply");
        require_preview_start(dialog, "driver", 2.5, 1.25);
        require_preview_start(dialog, "dependent", 10.5, 1.25);
        require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(), "preview must not mutate live geometry or history");
        capture(dialog, "room-relationship-area-cancel.png");
        child<QDialogButtonBox>(dialog, "roomRelationshipPropagationButtons").button(QDialogButtonBox::Cancel)->click();
    });
    require(window.document().revision() == before.revision() && window.document().snapshot().entities() == before.entities(),
        "Cancel must preserve both owners, dimensions, receipts and relationship record exactly");
    propagation_dialog(window, [&](QDialog& dialog) {
        set_move(dialog, "driver");
        require(apply_button(dialog).isEnabled(), "same valid move must remain applicable after Cancel");
        capture(dialog, "room-relationship-area-apply.png");
        apply_button(dialog).click();
        require(!dialog.isVisible(), "successful Apply must close the actual propagation dialog");
    });
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1 && after.history().size() == before.history().size() + 1,
        "driver and all dependents must commit in one revision and history operation");
    require_translation(before.entities().at("driver"), after.entities().at("driver"), 2.5, 1.25);
    require_translation(before.entities().at("dependent"), after.entities().at("dependent"), 2.5, 1.25);
    require(after.entities().at("relationships") == before.entities().at("relationships"),
        "geometry Apply must preserve the exact declared relationship graph");
    for (const auto id : {"driver-dimension", "dependent-dimension"}) {
        const auto original = *decode_boundary_dimension_entity(before.entities().at(id)).dimension;
        const auto moved = *decode_boundary_dimension_entity(after.entities().at(id)).dimension;
        require(moved.boundary_id == original.boundary_id && moved.segment_id == original.segment_id &&
                near(moved.text_position.x, original.text_position.x + 2.5) &&
                near(moved.text_position.y, original.text_position.y + 1.25) &&
                near(moved.resolve(after.entities().at(moved.boundary_id)).segment_length(), 4),
            "dimensions on BOTH moved owners must retain stable targets and translate their text anchors");
    }
    require(!validate_boundary_integrity(after.entities()), "moved receipts and dimensions must pass integrity replay");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
        "one Undo must restore the exact driver, dependent, receipts and dimension anchors");
    require(window.redoCommand() && window.document().snapshot().entities() == after.entities(),
        "one Redo must restore the exact complete movement");
    const auto project = directory.filePath("room-move.bldproj");
    require(window.saveProjectAs(project), "complete room movement and history must save");
    {
        MainWindow read_only({}, nullptr, directory.filePath("read-only-library.json"));
        require(read_only.openProject(project) && !read_only.document().is_editable(),
            "second opener must honor the writer's existing project ownership");
        const auto locked = read_only.document().snapshot();
        read_only.showRoomRelationships();
        require(!read_only.findChild<QDialog*>("roomRelationshipsDialog") &&
                read_only.lastError().contains("read-only", Qt::CaseInsensitive) && !read_only.undoCommand() &&
                read_only.document().snapshot().entities() == locked.entities() &&
                read_only.document().revision() == locked.revision(), "read-only relationships and history must refuse unchanged");
    }
    require(window.createNewProject(), "writer must release ownership before an editable reopen");
    MainWindow reopened({}, nullptr, directory.filePath("reopened-library.json"));
    require(reopened.openProject(project) && reopened.document().is_editable() &&
            reopened.document().snapshot().entities() == after.entities(), "save/reopen must retain exact geometry, evidence and metadata");
    require(reopened.undoCommand() && reopened.document().snapshot().entities() == before.entities() &&
            reopened.redoCommand() && reopened.document().snapshot().entities() == after.entities(),
        "persisted history must Undo/Redo BOTH owners atomically after ownership-safe reopen");
}

void test_invalid_conflicting_and_stale_moves() {
    QTemporaryDir directory;
    require(directory.isValid(), "refusal fixtures need a temporary directory");
    MainWindow conflict(area_document(true), nullptr, directory.filePath("conflict-library.json"));
    prepare_window(conflict);
    const auto conflict_before = conflict.document().snapshot();
    propagation_dialog(conflict, [&](QDialog& dialog) {
        set_move(dialog, "driver");
        require(!apply_button(dialog).isEnabled() &&
                child<QLabel>(dialog, "roomRelationshipPropagationStatus").text().contains("conflict", Qt::CaseInsensitive),
            "a dependent with differing declared driver transforms must block the complete move");
        capture(dialog, "room-relationship-conflicting-drivers.png");
    });
    require(conflict.document().revision() == conflict_before.revision() &&
            conflict.document().snapshot().entities() == conflict_before.entities(), "conflicting move must leave every owner unchanged");

    MainWindow stale(area_document(), nullptr, directory.filePath("stale-library.json"));
    prepare_window(stale);
    const auto initial = stale.document().snapshot();
    propagation_dialog(stale, [&](QDialog& dialog) {
        set_move(dialog, "driver");
        child<QLineEdit>(dialog, "roomRelationshipPropagationOffsetX").setText("not a distance");
        require(!apply_button(dialog).isEnabled() &&
                !child<QLabel>(dialog, "roomRelationshipPropagationStatus").text().isEmpty(),
            "invalid offset must refuse Apply with an explanation");
        require(stale.document().snapshot().entities() == initial.entities() && stale.document().revision() == initial.revision(),
            "invalid input must leave live geometry and history unchanged");
        set_move(dialog, "driver");
        require(apply_button(dialog).isEnabled(), "corrected input must restore a valid candidate");
        stale.document().apply(ApplyEntityChanges{stale.document().revision(),
            {EntityChange::upsert(receipt_rectangle("concurrent-area", "room_boundary", 30))}, {}, "Concurrent area"});
        const auto concurrent = stale.document().snapshot();
        apply_button(dialog).click();
        require(dialog.isVisible() && !apply_button(dialog).isEnabled() &&
                !child<QLabel>(dialog, "roomRelationshipPropagationStatus").text().isEmpty() &&
                stale.document().revision() == concurrent.revision() &&
                stale.document().snapshot().entities() == concurrent.entities(),
            "stale Apply must refuse without moving either owner or overwriting concurrent work");
        capture(dialog, "room-relationship-stale-refusal.png");
    });
}

void test_wall_driver_with_hosted_opening() {
    QTemporaryDir directory;
    require(directory.isValid(), "physical relationship fixture needs a temporary directory");
    MainWindow window({}, nullptr, directory.filePath("wall-library.json"));
    prepare_window(window);
    const auto wall = window.createStraightWall({0, 0}, {6, 0}, "exterior");
    require(!wall.isEmpty() && window.selectEntity(wall), "physical fixture must create/select its actual wall driver");
    const auto opening = window.createHostedOpening("door", "1 m", "1 m", "0 m", "2 m");
    require(!opening.isEmpty(), "physical fixture must create a real hosted door");
    const auto room = receipt_rectangle("wall-dependent", "room_boundary", 8);
    const auto relationships = relation_entity({{wall.toStdString(), K::architectural_wall},
                                                {room.id, K::room_boundary}},
                                               {{room.id, wall.toStdString(), R::follows}});
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(room), EntityChange::upsert(relationships)}, {}, "Wall relationship fixture"});
    require(window.selectEntity(wall), "select the physical reference before propagation");
    const auto before = window.document().snapshot();
    propagation_dialog(window, [&](QDialog& dialog) {
        set_move(dialog, wall.toStdString(), "3 m", "2 m");
        require(apply_button(dialog).isEnabled(), "wall with an attached opening must support rigid relationship preview");
        require_preview_start(dialog, wall.toStdString(), 3, 2);
        require_preview_start(dialog, room.id, 11, 2);
        require(window.document().snapshot().entities() == before.entities(), "physical preview must retain the live wall/opening/room");
        capture(dialog, "room-relationship-physical-wall-apply.png");
        apply_button(dialog).click();
        require(!dialog.isVisible(), "physical relationship Apply must close after one atomic commit");
    });
    const auto after = window.document().snapshot();
    const auto& baseline = after.entities().at(wall.toStdString()).properties.at("baseline");
    require(after.revision() == before.revision() + 1 &&
            near(baseline.at("start")[0].get<double>(), 3) && near(baseline.at("start")[1].get<double>(), 2) &&
            near(baseline.at("end")[0].get<double>(), 9) && near(baseline.at("end")[1].get<double>(), 2),
        "physical relationship Apply must move the wall driver itself as well as its dependent room");
    require_translation(before.entities().at(room.id), after.entities().at(room.id), 3, 2);
    require(after.entities().at(opening.toStdString()) == before.entities().at(opening.toStdString()),
        "rigid wall movement must retain the hosted opening's identity, host binding and local distances exactly");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
            window.redoCommand() && window.document().snapshot().entities() == after.entities(),
        "wall, attached opening and dependent room must share exact atomic Undo/Redo");
}

void test_independent_and_cross_record_moves() {
    QTemporaryDir directory;
    auto independent = area_document();
    independent->apply(ApplyEntityChanges{independent->revision(), {EntityChange::upsert(
        relation_entity({{"driver", K::room_boundary}, {"dependent", K::appraisal_measurement_boundary}},
            {{"dependent", "driver", R::independent}}))}, {}, "Independent areas"});
    MainWindow solo(independent, nullptr, directory.filePath("independent-library.json"));
    prepare_window(solo);
    const auto before = solo.document().snapshot();
    propagation_dialog(solo, [&](QDialog& dialog) {
        set_move(dialog, "driver");
        child<QLineEdit>(dialog, "roomRelationshipPropagationRotation").setText("90");
        require(apply_button(dialog).isEnabled(), "independent driver rotation must remain usable without dependents");
        capture(dialog, "room-relationship-independent-rotation.png");
        apply_button(dialog).click();
        require(!dialog.isVisible(), "independent rotation must commit");
    });
    const auto after = solo.document().snapshot();
    const auto original = decode_identified_boundary_entity(before.entities().at("driver"));
    const auto rotated = decode_identified_boundary_entity(after.entities().at("driver"));
    const PlanarTransform rotation{{2, 1.5}, std::acos(-1.0) / 2, false, false, {2.5, 1.25}};
    for (std::size_t index = 0; index < original.segments.size(); ++index) {
        const auto expected = transform_segment(original.segments[index].segment, rotation);
        require(near(rotated.segments[index].segment.start.x, expected.start.x) &&
            near(rotated.segments[index].segment.start.y, expected.start.y), "rotation must use the driver-centered pivot");
    }
    require(after.entities().at("dependent") == before.entities().at("dependent") &&
        after.entities().at("dependent-dimension") == before.entities().at("dependent-dimension"),
        "independent area and dimensions must remain exactly unchanged");
    require(solo.undoCommand() && solo.document().snapshot().entities() == before.entities(), "independent rotation is one reversible edit");

    auto chain = area_document();
    auto second = relation_entity({{"dependent", K::appraisal_measurement_boundary}, {"third", K::room_boundary}},
        {{"third", "dependent", R::follows}});
    second.id = "relationships-second";
    chain->apply(ApplyEntityChanges{chain->revision(), {EntityChange::upsert(receipt_rectangle("third", "room_boundary", 16)),
        EntityChange::upsert(second)}, {}, "Second relationship model"});
    MainWindow joined(chain, nullptr, directory.filePath("cross-record-library.json"));
    prepare_window(joined);
    const auto joined_before = joined.document().snapshot();
    propagation_dialog(joined, [&](QDialog& dialog) {
        set_move(dialog, "driver");
        require(apply_button(dialog).isEnabled(), "dependency chains across separate records must propagate coherently");
        require_preview_start(dialog, "third", 18.5, 1.25);
        capture(dialog, "room-relationship-cross-record-chain.png");
        apply_button(dialog).click();
        require(!dialog.isVisible(), "cross-record chain must commit once");
    });
    const auto joined_after = joined.document().snapshot();
    for (const auto id : {"driver", "dependent", "third"})
        require_translation(joined_before.entities().at(id), joined_after.entities().at(id), 2.5, 1.25);
    require(joined_after.revision() == joined_before.revision() + 1 && joined.undoCommand() &&
        joined.document().snapshot().entities() == joined_before.entities(), "cross-record chain is one reversible operation");
    auto cycle = relation_entity({{"driver", K::room_boundary}, {"dependent", K::appraisal_measurement_boundary}},
        {{"driver", "dependent", R::follows}});
    cycle.id = "relationships-second";
    const auto cycle_before = joined.document().snapshot();
    bool cycle_rejected = false;
    try {
        joined.document().apply(ApplyEntityChanges{joined.document().revision(),
            {EntityChange::upsert(cycle)}, {}, "Cross-record cycle"});
    } catch (const DocumentError&) { cycle_rejected = true; }
    require(cycle_rejected && joined.document().snapshot().entities() == cycle_before.entities() &&
            joined.document().revision() == cycle_before.revision(),
        "a cycle hidden across separate records must refuse document admission without mutation");
}

void test_exterior_measurement_driver() {
    QTemporaryDir directory;
    MainWindow window({}, nullptr, directory.filePath("exterior-library.json"));
    prepare_window(window);
    const QStringList walls{window.createStraightWall({0,0},{4,0},"exterior"),
        window.createStraightWall({4,0},{4,3},"exterior"), window.createStraightWall({4,3},{0,3},"exterior"),
        window.createStraightWall({0,3},{0,0},"exterior")};
    for (qsizetype index = 0; index < walls.size(); ++index)
        require(!walls[index].isEmpty() && window.selectEntity(walls[index], index != 0), "select the source shell walls");
    const auto area = window.createMeasurementBoundaryFromSelectedWalls();
    require(!area.isEmpty(), "create a real exterior measurement from its physical sources");
    const auto room = receipt_rectangle("exterior-dependent", "room_boundary", 8);
    window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(room),
        EntityChange::upsert(relation_entity({{area.toStdString(), K::appraisal_measurement_boundary}, {room.id, K::room_boundary}},
            {{room.id, area.toStdString(), R::follows}}))}, {}, "Exterior relationship"});
    const auto before = window.document().snapshot();
    propagation_dialog(window, [&](QDialog& dialog) {
        set_move(dialog, area.toStdString());
        require(apply_button(dialog).isEnabled(), "source-backed area and dependent must preview together");
        capture(dialog, "room-relationship-exterior-sources.png");
        apply_button(dialog).click();
        require(!dialog.isVisible(), "complete source-backed move must commit");
    });
    const auto after = window.document().snapshot();
    require(wall_measurement_source_current(after, after.entities().at(area.toStdString())), "moved exterior remains current with its physical walls");
    for (const auto& id : walls) {
        const auto& old = before.entities().at(id.toStdString()).properties.at("baseline");
        const auto& moved = after.entities().at(id.toStdString()).properties.at("baseline");
        require(near(moved.at("start")[0].get<double>(), old.at("start")[0].get<double>() + 2.5) &&
            near(moved.at("start")[1].get<double>(), old.at("start")[1].get<double>() + 1.25), "supporting walls move with the exterior driver");
    }
    require_translation(before.entities().at(room.id), after.entities().at(room.id), 2.5, 1.25);
    require(after.revision() == before.revision() + 1 && window.undoCommand() &&
        window.document().snapshot().entities() == before.entities(), "physical sources, exterior and dependent share one exact undo");
}

void sync_whole_wall_references(MainWindow& window, const std::string& wall,
                                const std::vector<std::string>& members) {
    std::exception_ptr failure;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("roomRelationshipsDialog");
        try {
            require(dialog != nullptr, "the actual relationship editor must open for Sync");
            child<QPushButton>(*dialog, "syncRoomRelationships").click();
            const auto model = window.document().snapshot().entities().at("relationships").properties.at("model");
            const auto decoded = RoomRelationshipSnapshot::from_json(model);
            require(decoded.schema_version() == 2, "Sync must preserve schema-two interpretation");
            const auto reference = std::find_if(decoded.references().begin(), decoded.references().end(),
                [&](const auto& value) { return value.id == wall; });
            require(reference != decoded.references().end() && room_reference_wall_ids(*reference) == members,
                "Sync must preserve the ordered whole-wall reference exactly");
            for (const char* name : {"roomRelationshipSource", "roomRelationshipTarget"}) {
                auto& picker = child<QComboBox>(*dialog, name);
                require(picker.findData(QString::fromStdString(wall)) >= 0, "Sync must offer the whole wall");
                for (std::size_t index = 1; index < members.size(); ++index)
                    require(picker.findData(QString::fromStdString(members[index])) < 0,
                        "a whole wall's member cannot also be offered as an independent reference");
            }
            capture(*dialog, "room-relationship-whole-wall-sync.png");
        } catch (...) { failure = std::current_exception(); }
        if (dialog) dialog->reject();
    });
    window.showRoomRelationships();
    if (failure) std::rethrow_exception(failure);
}

void test_split_whole_wall_move_and_history(bool curved) {
    QTemporaryDir directory;
    MainWindow window({}, nullptr, directory.filePath("split-library.json"));
    prepare_window(window);
    const auto wall = curved ? window.createCurvedWall({0,0}, {6,0}, "90 deg", "exterior")
                             : window.createStraightWall({0,0}, {6,0}, "exterior");
    require(!wall.isEmpty() && window.selectEntity(wall), "create and select the whole wall driver");
    const auto opening = window.createHostedOpening("door", "4.8 m", "0.5 m", "0 m", "2 m");
    require(!opening.isEmpty(), "place a hosted door clear of both proposed seam stations");
    const auto room = receipt_rectangle("split-dependent", "room_boundary", 8);
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(room), EntityChange::upsert(relation_entity(
            {{wall.toStdString(), K::architectural_wall}, {room.id, K::room_boundary}},
            {{room.id, wall.toStdString(), R::follows}}))}, {}, "Whole-wall relationship"});
    require(window.selectEntity(wall), "select the referenced wall before inserting a point");
    require(window.insertSelectedBoundaryVertex("baseline", "0.4"),
        "a whole-wall relationship must survive the actual wall point insertion command");
    auto model = RoomRelationshipSnapshot::from_json(
        window.document().snapshot().entities().at("relationships").properties.at("model"));
    auto reference = std::find_if(model.references().begin(), model.references().end(),
        [&](const auto& value) { return value.id == wall.toStdString(); });
    require(reference != model.references().end() && reference->wall_members.size() == 2,
        "first split must retain one whole-wall reference with two physical members");
    const auto second = reference->wall_members[1];
    require(window.selectEntity(QString::fromStdString(second)) &&
            window.insertSelectedBoundaryVertex("baseline", "0.5"),
        "splitting a non-head member must preserve the complete logical span");
    model = RoomRelationshipSnapshot::from_json(
        window.document().snapshot().entities().at("relationships").properties.at("model"));
    reference = std::find_if(model.references().begin(), model.references().end(),
        [&](const auto& value) { return value.id == wall.toStdString(); });
    require(reference != model.references().end() && reference->wall_members.size() == 3 &&
            reference->wall_members[1] == second,
        "recursive split must splice the new child in native baseline order");
    const auto members = reference->wall_members;
    sync_whole_wall_references(window, wall.toStdString(), members);
    const auto before = window.document().snapshot();
    propagation_dialog(window, [&](QDialog& dialog) {
        set_move(dialog, wall.toStdString(), "2 m", "1 m");
        require(apply_button(dialog).isEnabled(), "all physical wall members and dependent room must preview together");
        auto& picker = child<QComboBox>(dialog, "roomRelationshipPropagationDriver");
        for (std::size_t index = 1; index < members.size(); ++index)
            require(picker.findData(QString::fromStdString(members[index])) < 0,
                "propagation must offer one logical whole wall without child aliases");
        require(window.document().snapshot().entities() == before.entities(), "whole-wall preview must not edit the project");
        child<QDialogButtonBox>(dialog, "roomRelationshipPropagationButtons").button(QDialogButtonBox::Cancel)->click();
    });
    require(window.document().snapshot().entities() == before.entities(), "Cancel must leave all split geometry unchanged");
    propagation_dialog(window, [&](QDialog& dialog) {
        set_move(dialog, wall.toStdString(), "2 m", "1 m");
        require(apply_button(dialog).isEnabled(), "whole-wall move must remain applicable after Cancel");
        capture(dialog, curved ? "room-relationship-curved-whole-wall.png" : "room-relationship-straight-whole-wall.png");
        apply_button(dialog).click();
        require(!dialog.isVisible(), "whole-wall Apply must close after one complete command");
    });
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1, "all wall members and the dependent share one revision");
    for (const auto& member : members) {
        const auto& original = before.entities().at(member).properties.at("baseline");
        const auto& moved = after.entities().at(member).properties.at("baseline");
        for (const char* endpoint : {"start", "end"})
            require(near(moved.at(endpoint)[0].get<double>(), original.at(endpoint)[0].get<double>() + 2) &&
                    near(moved.at(endpoint)[1].get<double>(), original.at(endpoint)[1].get<double>() + 1),
                "every member endpoint must receive the complete requested move");
        require(moved.at("sweep_radians") == original.at("sweep_radians"), "movement must retain each analytical sweep");
    }
    require_translation(before.entities().at(room.id), after.entities().at(room.id), 2, 1);
    require(after.entities().at(opening.toStdString()) == before.entities().at(opening.toStdString()) &&
            after.entities().at("relationships") == before.entities().at("relationships"),
        "movement must retain hosted binding and the exact whole-wall relationship model");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
            window.redoCommand() && window.document().snapshot().entities() == after.entities(),
        "all members and dependent geometry must have one exact Undo/Redo");
    const auto project = directory.filePath(curved ? "curved-whole-wall.bldproj" : "straight-whole-wall.bldproj");
    require(window.saveProjectAs(project) && window.createNewProject(), "save and release whole-wall project ownership");
    MainWindow reopened({}, nullptr, directory.filePath("reopened-split-library.json"));
    require(reopened.openProject(project) && reopened.document().is_editable() &&
            reopened.document().snapshot().entities() == after.entities(), "whole-wall membership and geometry must reopen exactly");
    require(reopened.undoCommand() && reopened.document().snapshot().entities() == before.entities() &&
            reopened.redoCommand() && reopened.document().snapshot().entities() == after.entities(),
        "persisted history must retain the complete whole-wall move");
}
} // namespace

int main(int argc, char** argv) {
    testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("Vertex-tests");
    QCoreApplication::setApplicationName("Vertex-room-relationship-move-test-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf") >= 0, "bundled Inter must load for native preview captures");
        application.setFont(QFont("Inter", 10));
        const auto arguments = application.arguments();
        const bool area_only = arguments.contains("--area-only");
        const bool refusal_only = arguments.contains("--refusal-only");
        const bool wall_only = arguments.contains("--wall-only");
        if (!refusal_only && !wall_only) test_area_move_and_history();
        if (!area_only && !wall_only) test_invalid_conflicting_and_stale_moves();
        if (!area_only && !refusal_only) test_wall_driver_with_hosted_opening();
        if (!area_only && !refusal_only && !wall_only) test_independent_and_cross_record_moves();
        if (!area_only && !refusal_only && !wall_only) test_exterior_measurement_driver();
        if (!area_only && !refusal_only && !wall_only) {
            test_split_whole_wall_move_and_history(false);
            test_split_whole_wall_move_and_history(true);
        }
        std::cout << "room_relationship_move_desktop_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "room_relationship_move_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
