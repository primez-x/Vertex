#include "sketch/desktop/main_window.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
using namespace sketch;
using sketch::desktop::MainWindow;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json policy() {
    return {{"policy_kind", "ansi_z765_2021"}, {"version", 2},
        {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"},
        {"ansi", {{"interior_inspected", true}, {"direct_measurement", true}, {"acquisition_increment", "inch"},
            {"limitations_statement", ""}}}};
}
Json flat_facts() {
    return {{"finish", "finished"}, {"access", "direct_interior"}, {"area_use", "dwelling"},
        {"boundary_role", "measured_area"}, {"ansi", {{"year_round_suitable", true},
            {"finish_matches_dwelling", true}, {"dwelling_identity", "primary"},
            {"ceiling", {{"kind", "flat"}, {"minimum_height_m", 2.4384}}}}}};
}
Entity area(const std::string& id, double lo, double hi) {
    const Json square = Json::array({{{"start", {lo, lo}}, {"end", {hi, lo}}, {"sweep_radians", 0}},
        {{"start", {hi, lo}}, {"end", {hi, hi}}, {"sweep_radians", 0}},
        {{"start", {hi, hi}}, {"end", {lo, hi}}, {"sweep_radians", 0}},
        {{"start", {lo, hi}}, {"end", {lo, lo}}, {"sweep_radians", 0}}});
    return upgrade_legacy_boundary_entity({id, "measurement_boundary", {{"property_id", "p"},
        {"building_id", "b"}, {"floor_id", "f"}, {"layer_id", "l"}, {"boundary", square},
        {"name", id}, {"classification", "above_grade_finished"}, {"factor", 1},
        {"appraisal_category", "above_grade_finished"}, {"appraisal_facts", flat_facts()}}, false,
        {{"vendor", {{"marker", id}, {"style", Json::array({3, 17, "retain"})}}}}});
}
std::shared_ptr<Document> fixture() {
    auto outer = area("floor-area", 0, 10), room = area("room-area", 2, 8), void_area = area("void-area", 4, 6);
    outer.properties["deduction_ids"] = {room.id}; room.properties["deduction_ids"] = {void_area.id};
    void_area.properties["appraisal_facts"] = {{"boundary_role", "other_void"}};
    void_area.properties.erase("appraisal_category");
    return std::make_shared<Document>(Document::create({
        {"p", "property", {{"name", "Nested ANSI copy"}, {"calculation_workflow", "appraisal"}, {"appraisal_policy", policy()}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}, {"appraisal_facts", {{"grade", "above"}, {"ansi", {{"any_part_below_grade", false}}}}}}, false},
        {"l", "layer", {{"floor_id", "f"}, {"name", "Measured"}}, false}, outer, room, void_area}));
}
void prepare(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1280, 900); window.show();
    window.setMetricUnits(true); QApplication::processEvents();
}
void expect_total(const DocumentSnapshot& snapshot, double expected, const std::string& property_id = "p") {
    const auto report = build_appraisal_document_report(snapshot, property_id, AreaUnit::square_metre);
    if (!report.qualified || !report.calculation) {
        std::string message = "Nested ANSI fixture must qualify";
        for (const auto& issue : report.issues) message += "; " + issue;
        throw std::runtime_error(message);
    }
    require(std::abs(report.calculation->property.gla().total.square_metres - expected) < 1e-8,
        "nested ANSI GLA counts outer64 plus room32 exactly once");
}
Entity stroke(const std::string& id, double lo, double hi) {
    const std::vector<Vec2> points{{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}, {lo, lo}};
    MeasurementLinework model; model.stroke_id = id; model.anchor = points.front(); model.closed = true;
    for (std::size_t i = 1; i < points.size(); ++i) {
        ConstructionReceipt receipt; receipt.segment_id = id + ":edge" + std::to_string(i);
        receipt.kind = BoundaryConstructionKind::line_to_point; receipt.start = points[i - 1]; receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, id + ":vertex" + std::to_string(i - 1),
            i == 4 ? id + ":vertex0" : id + ":vertex" + std::to_string(i), receipt});
    }
    return {id, "measurement_linework", {{"property_id", "p"}, {"building_id", "b"},
        {"floor_id", "f"}, {"layer_id", "l"}, {"model", encode_measurement_linework_model(model)}}, true};
}
std::vector<Entity> fixture_entities() {
    const auto snapshot = fixture()->snapshot(); std::vector<Entity> entities;
    for (const auto& [id, entity] : snapshot.entities()) entities.push_back(entity);
    return entities;
}
Entity& find(std::vector<Entity>& entities, const std::string& id) {
    for (auto& entity : entities) if (entity.id == id) return entity;
    throw std::runtime_error("fixture entity is present");
}
std::shared_ptr<Document> sourced_fixture(bool sloped = false) {
    auto entities = fixture_entities();
    for (const auto& [id, lo, hi] : std::vector<std::tuple<std::string, double, double>>{
            {"floor-area", 0, 10}, {"room-area", 2, 8}, {"void-area", 4, 6}}) {
        const auto source_id = id + "-source"; auto lineage = Json::array();
        for (int edge = 1; edge <= 4; ++edge)
            lineage.push_back(Json::array({{{"owner_id", source_id}, {"segment_id", source_id + ":edge" + std::to_string(edge)},
                {"parameter_start", 0}, {"parameter_end", 1}, {"reversed", false}}}));
        find(entities, id).extensions["measurement_linework_sources"] = lineage;
        entities.push_back(stroke(source_id, lo, hi));
    }
    if (sloped) {
        const auto room = boundary_geometry(decode_identified_boundary_entity(find(entities, "room-area")));
        const auto low = boundary_geometry(decode_identified_boundary_entity(find(entities, "void-area")));
        find(entities, "room-area").properties["appraisal_facts"]["ansi"]["ceiling"] = {
            {"kind", "sloped"}, {"complete_room_observed", true}, {"at_least_7ft_area_m2", 20},
            {"room_floor_area_m2", 36}, {"room_boundary_id", "room-area"}, {"below_5ft_deduction_ids", {"void-area"}},
            {"source_geometry_sha256", appraisal_ceiling_geometry_digest(room, {{"void-area", low}})}};
    }
    PresentationOverride appearance; appearance.target_kind = "area"; appearance.target_id = "room-area";
    appearance.style.stroke_color = "#315a8c"; appearance.style.fill_color = "#dce6f2";
    appearance.style.fill_pattern = "hatch"; appearance.style.bold = true;
    appearance.paper_line_width_mm = 0.45; appearance.hatch_scale = 1.7; appearance.plan_label_offset = Vec2{0.7, -0.4};
    AnnotationState annotations; annotations.overrides.push_back(appearance);
    entities.push_back(make_annotation_entity("copy-style", annotations));
    return std::make_shared<Document>(Document::create(entities));
}
void author_dimensions(MainWindow& window) {
    for (const auto* id : {"floor-area", "room-area", "void-area"}) {
        const auto model = decode_identified_boundary_entity(window.document().snapshot().entities().at(id));
        const auto length = window.createLengthDimension(QString::fromLatin1(id),
            QString::fromStdString(model.segments.front().segment_id), model.segments.front().segment.start);
        const auto area_dimension = window.createAreaDimension(QString::fromLatin1(id), model.segments.front().segment.end);
        require(!length.isEmpty() && !area_dimension.isEmpty(), "native dimensions author each nested owner");
        require(window.editBoundaryDimension(length, "1 m", "-1 m", "3.5", "#315a8c", true, true, true, "17"),
            "native nested dimension presentation is authored");
    }
}
using CopyMap = std::map<std::string, Entity>;
CopyMap copied_areas(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    CopyMap copied;
    for (const auto& [id, entity] : after.entities())
        if (!before.entities().contains(id) && entity.type == "measurement_boundary")
            require(copied.emplace(entity.extensions.at("vendor").at("marker").get<std::string>(), entity).second,
                "each nested owner is copied exactly once");
    require(copied.size() == 3, "independent copy contains exactly floor room and void owners");
    require(copied.at("floor-area").properties.at("deduction_ids") == std::vector<std::string>{copied.at("room-area").id} &&
        copied.at("room-area").properties.at("deduction_ids") == std::vector<std::string>{copied.at("void-area").id} &&
        !copied.at("void-area").properties.contains("deduction_ids"), "copied links retain the exact immediate graph");
    return copied;
}
void originals_unchanged(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    for (const auto& [id, original] : before.entities())
        require(after.entities().at(id) == original, "independent copy preserves every original entity exactly");
}
void atomic_history(MainWindow& window, const DocumentSnapshot& before, const DocumentSnapshot& after) {
    require(after.revision() == before.revision() + 1 && after.history().size() == before.history().size() + 1,
        "whole graph operation commits one history command");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == after.entities(),
        "one Undo and Redo restore exact graph sources facts styles and dimensions");
}
void check_full_copy(const DocumentSnapshot& before, const DocumentSnapshot& after, bool exact_facts) {
    originals_unchanged(before, after); const auto copied = copied_areas(before, after);
    std::set<std::string> old_geometry_ids, copied_source_ids;
    for (const auto& [id, entity] : before.entities()) {
        old_geometry_ids.insert(id);
        if (entity.type == "measurement_boundary")
            for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
                old_geometry_ids.insert(edge.segment_id); old_geometry_ids.insert(edge.start_vertex_id); old_geometry_ids.insert(edge.end_vertex_id);
            }
        if (entity.type == "measurement_linework") {
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            require(decoded.supported(), "original measured source is supported");
            for (const auto& edge : decoded.model->edges) {
                old_geometry_ids.insert(edge.segment_id); old_geometry_ids.insert(edge.start_vertex_id); old_geometry_ids.insert(edge.end_vertex_id);
            }
        }
    }
    const auto checks = measurement_linework_source_checks(after.entities());
    for (const auto& [original_id, clone] : copied) {
        const auto& original = before.entities().at(original_id);
        require(clone.extensions.at("vendor") == original.extensions.at("vendor"), "opaque vendor style metadata is retained exactly");
        for (const auto* key : {"name", "classification", "factor"})
            require(clone.properties.at(key) == original.properties.at(key), "copy retains authored name classification and factor");
        if (exact_facts) require(clone.properties.at("appraisal_facts") == original.properties.at("appraisal_facts"),
            "flat eligibility and raw exclusion facts are retained exactly");
        const auto model = decode_identified_boundary_entity(clone);
        const auto old_model = decode_identified_boundary_entity(original);
        const Vec2 delta{model.segments.front().segment.start.x - old_model.segments.front().segment.start.x,
            model.segments.front().segment.start.y - old_model.segments.front().segment.start.y};
        require(model.segments.size() == old_model.segments.size(), "copy retains complete analytical geometry");
        for (std::size_t i = 0; i < model.segments.size(); ++i) {
            const auto& edge = model.segments[i]; const auto& old_edge = old_model.segments[i];
            require(!old_geometry_ids.contains(edge.segment_id) && !old_geometry_ids.contains(edge.start_vertex_id) &&
                !old_geometry_ids.contains(edge.end_vertex_id), "copied owner gets fresh segment and vertex identities");
            require(std::abs(edge.segment.start.x - old_edge.segment.start.x - delta.x) < 1e-8 &&
                std::abs(edge.segment.start.y - old_edge.segment.start.y - delta.y) < 1e-8 &&
                std::abs(edge.segment.end.x - old_edge.segment.end.x - delta.x) < 1e-8 &&
                std::abs(edge.segment.end.y - old_edge.segment.end.y - delta.y) < 1e-8 &&
                edge.segment.sweep_radians == old_edge.segment.sweep_radians, "whole nested graph keeps analytical shape under one translation");
        }
        require(checks.at(clone.id).current && checks.at(original_id).current, "copied and original measured lineages are current independently");
        for (const auto& uses : clone.extensions.at("measurement_linework_sources")) for (const auto& use : uses) {
            const auto source_id = use.at("owner_id").get<std::string>(); copied_source_ids.insert(source_id);
            require(!before.entities().contains(source_id) && after.entities().at(source_id).type == "measurement_linework",
                "every copied source is actual independent measured geometry");
        }
    }
    require(copied_source_ids.size() == 3, "complete nested copy includes all three measured source owners");
    for (const auto& id : copied_source_ids) {
        const auto decoded = decode_measurement_linework_model(after.entities().at(id).properties.at("model"));
        require(decoded.supported(), "copied measured source remains supported");
        for (const auto& edge : decoded.model->edges)
            require(!old_geometry_ids.contains(edge.segment_id) && !old_geometry_ids.contains(edge.start_vertex_id) &&
                !old_geometry_ids.contains(edge.end_vertex_id), "copied measured sources get fresh edge and vertex identities");
    }
    int dimensions = 0; bool appearance = false;
    for (const auto& [id, entity] : after.entities()) {
        if (before.entities().contains(id)) continue;
        if (entity.type == "dimension") {
            ++dimensions; const auto decoded = decode_boundary_dimension_entity(entity); require(decoded.supported(), "copied native dimension remains supported");
            const auto& dimension = *decoded.dimension; bool matched = false;
            for (const auto& [old_id, old] : before.entities()) if (old.type == "dimension") {
                const auto source = decode_boundary_dimension_entity(old);
                if (dimension.boundary_id != copied.at(source.dimension->boundary_id).id || dimension.kind != source.dimension->kind) continue;
                matched = true; require(dimension.presentation == source.dimension->presentation, "copied dimension retains exact authored text style");
                const auto old_value = source.dimension->resolve(before.entities().at(source.dimension->boundary_id));
                const auto new_value = dimension.resolve(after.entities().at(dimension.boundary_id));
                require(std::abs(old_value.segment_length_metres - new_value.segment_length_metres) < 1e-8 &&
                    std::abs(old_value.area_square_metres - new_value.area_square_metres) < 1e-8, "copied dimensions resolve from copied analytical owners");
            }
            require(matched, "every copied dimension retains a real copied target");
        }
        if (entity.type == kAnnotationEntityType)
            for (const auto& record : entity.properties.at("state").at("overrides"))
                if (record.at("target_id") == copied.at("room-area").id) {
                    auto expected = before.entities().at("copy-style").properties.at("state").at("overrides").at(0);
                    expected["target_id"] = copied.at("room-area").id;
                    require(record == expected, "area presentation style and label offset survive with the copied target"); appearance = true;
                }
    }
    require(dimensions == 6 && appearance, "nested copy retains all six authored dimensions and actual area presentation");
}
void save_facts_dialog(MainWindow& window, bool confirm_room = false) {
    std::exception_ptr failure; bool opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("appraisalFactsDialog");
        try {
            require(dialog, "actual Facts dialog opens"); opened = true;
            if (confirm_room) {
                auto* confirm = dialog->findChild<QCheckBox*>("ansiConfirmRoomGeometry");
                require(confirm, "native copied room confirmation control exists"); confirm->setChecked(true);
            }
            auto* buttons = dialog->findChild<QDialogButtonBox*>("appraisalFactsButtons");
            require(buttons, "actual Facts save control exists"); buttons->button(QDialogButtonBox::Save)->click();
            require(dialog->result() == QDialog::Accepted, "Facts saves retained nested deduction links through native setter");
        } catch (...) { failure = std::current_exception(); if (dialog) dialog->reject(); }
    });
    window.showAppraisalFacts(); if (failure) std::rethrow_exception(failure); require(opened, "Facts callback ran");
}
void persist_and_report(MainWindow& window, const QString& directory, const char* name, const char* square_feet) {
    const auto saved = window.document().snapshot(); const auto selected = window.selectedEntityId();
    const auto project = directory + "/" + QString::fromLatin1(name) + ".bldproj";
    require(window.saveProjectAs(project) && window.openProject(project) && window.document().snapshot().entities() == saved.entities(),
        "native reopen retains exact copied identities graph sources facts styles and dimensions");
    const auto path = directory + "/" + QString::fromLatin1(name) + ".pdf";
    require(window.exportAppraisalReportPdf(path, "p", window.document().revision()), "native copied appraisal report PDF exports");
    QPdfDocument pdf; require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() > 0 &&
        !pdf.render(0, QSize(1000, 800)).isNull(), "actual copied appraisal PDF renders");
    QString text; for (int page = 0; page < pdf.pageCount(); ++page) text += pdf.getAllText(page).text();
    require(text.contains(QString::fromLatin1(square_feet)) && text.contains("Primary dwelling GLA"),
        "actual copied PDF agrees with canonical whole-square-foot GLA");
    const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        require(QDir().mkpath(capture), "create isolated copy capture directory");
        if (!selected.isEmpty()) require(window.selectEntity(selected), "restore actual copied owner for Details capture");
        window.fitView(); auto* tabs = window.findChild<QTabWidget*>("sidebarTabs"); require(tabs, "native copied Details sidebar exists");
        for (int index = 0; index < tabs->count(); ++index) if (tabs->tabText(index) == "Details") tabs->setCurrentIndex(index);
        QApplication::processEvents();
        require(window.grab().save(QDir(capture).filePath(QString::fromLatin1(name) + "-details.png")) &&
            pdf.render(0, QSize(1200, 900)).save(QDir(capture).filePath(QString::fromLatin1(name) + "-report.png")),
            "capture actual copied canvas Details and rendered appraisal PDF");
    }
}
void flat_graph_clone_clipboard_and_dialog() {
    QTemporaryDir directory; MainWindow window(sourced_fixture(), nullptr, directory.filePath("text-library.json")); prepare(window);
    author_dimensions(window); require(window.selectEntity("floor-area"), "select actual floor for retained nested Facts links");
    save_facts_dialog(window); const auto before = window.document().snapshot(); expect_total(before, 96);
    require(before.entities().at("floor-area").properties.at("deduction_ids") == std::vector<std::string>{"room-area"} &&
        before.entities().at("room-area").properties.at("deduction_ids") == std::vector<std::string>{"void-area"},
        "actual Facts Save retains both original nested links");
    require(window.selectEntity("floor-area") && window.transformSelectedBoundary("0", false, false, "20 m", "0 m", true),
        "native nested sourced graph clones with dependencies");
    const auto cloned = window.document().snapshot(); check_full_copy(before, cloned, true); expect_total(cloned, 192);
    atomic_history(window, before, cloned); require(window.undoCommand(), "remove clone before native clipboard test");
    const auto clipboard_before = window.document().snapshot();
    require(window.selectEntity("floor-area") && window.copySelection(), "actual clipboard accepts complete ANSI nested graph");
    require(window.document().snapshot().entities() == clipboard_before.entities() && window.document().revision() == clipboard_before.revision(),
        "Copy is read only");
    require(window.pasteSelection(), "actual sameproject clipboard paste admits complete nested graph");
    const auto pasted = window.document().snapshot(); check_full_copy(clipboard_before, pasted, true); expect_total(pasted, 192);
    atomic_history(window, clipboard_before, pasted); persist_and_report(window, directory.path(), "flat-copy", "2067");
}
void sloped_graph_requires_native_reconfirmation() {
    QTemporaryDir directory; MainWindow window(sourced_fixture(true), nullptr, directory.filePath("text-library.json")); prepare(window);
    author_dimensions(window); const auto before = window.document().snapshot(); expect_total(before, 96);
    require(window.selectEntity("floor-area") && window.copySelection() && window.pasteSelection(), "actual clipboard copies sloped nested graph");
    const auto pasted = window.document().snapshot(); check_full_copy(before, pasted, false); atomic_history(window, before, pasted);
    const auto copied = copied_areas(before, pasted); const auto room_id = copied.at("room-area").id, void_id = copied.at("void-area").id;
    const auto& original = before.entities().at("room-area").properties.at("appraisal_facts").at("ansi").at("ceiling");
    const auto& ceiling = copied.at("room-area").properties.at("appraisal_facts").at("ansi").at("ceiling");
    for (const auto* key : {"at_least_7ft_area_m2", "room_floor_area_m2", "source_geometry_sha256"})
        require(ceiling.at(key) == original.at(key), "copy retains original raw sloped observations and historical digest");
    require(ceiling.at("below_5ft_deduction_ids") == std::vector<std::string>{void_id} &&
        ceiling.value("room_boundary_id", std::string{}).empty() && !ceiling.value("complete_room_observed", false),
        "copied sloped references are independent and room confirmation is revoked");
    require(!build_appraisal_document_report(pasted, "p").qualified, "copied sloped evidence withholds property totals until explicit reconfirmation");
    require(window.selectEntity(QString::fromStdString(room_id)), "select real copied room for native confirmation");
    const auto before_confirmation = window.document().snapshot();
    save_facts_dialog(window, true); const auto confirmed = window.document().snapshot(); expect_total(confirmed, 192);
    for (const auto& [id, original_entity] : before.entities())
        if (confirmed.entities().at(id) != original_entity)
            throw std::runtime_error("Copied room reconfirmation changed source entity: " + id);
    require(confirmed.entities().at(room_id).properties.at("deduction_ids") == std::vector<std::string>{void_id},
        "native sloped confirmation retains actual copied void link");
    atomic_history(window, before_confirmation, confirmed); persist_and_report(window, directory.path(), "sloped-copy", "2067");
}
void sloped_root_retains_nested_general_exclusions() {
    std::vector<Entity> entities;
    const auto seed = sourced_fixture()->snapshot();
    for (const auto& [id, entity] : seed.entities()) entities.push_back(entity);
    auto& root = find(entities,"floor-area");
    find(entities,"room-area").properties["appraisal_facts"] = {{"boundary_role","other_void"}};
    const auto geometry = boundary_geometry(decode_identified_boundary_entity(root));
    const auto exclusion = boundary_geometry(decode_identified_boundary_entity(find(entities,"room-area")));
    root.properties["appraisal_facts"]["ansi"]["ceiling"] = {
        {"kind","sloped"},{"complete_room_observed",true},{"at_least_7ft_area_m2",60},
        {"room_floor_area_m2",100},{"room_boundary_id",root.id},{"below_5ft_deduction_ids",Json::array()},
        {"source_geometry_sha256",appraisal_ceiling_geometry_digest(geometry,{{"room-area",exclusion}})}};
    QTemporaryDir directory;
    MainWindow window(std::make_shared<Document>(Document::create(entities)),nullptr,directory.filePath("root-sloped.json"));
    prepare(window); const auto before = window.document().snapshot(); expect_total(before,64);
    require(window.selectEntity("floor-area") && window.copySelection() && window.pasteSelection(),
        "sloped root with nested general exclusions copies without flattening");
    const auto pasted = window.document().snapshot(); const auto copies = copied_areas(before,pasted);
    require(!build_appraisal_document_report(pasted,"p").qualified,
        "copied sloped root withholds totals before actual observation confirmation");
    require(window.selectEntity(QString::fromStdString(copies.at("floor-area").id)),"select copied sloped root");
    save_facts_dialog(window,true);
    const auto confirmed = window.document().snapshot(); expect_total(confirmed,128);
    originals_unchanged(before,confirmed);
    const auto& root_copy = confirmed.entities().at(copies.at("floor-area").id);
    require(root_copy.properties.at("deduction_ids") == std::vector<std::string>{copies.at("room-area").id} &&
        confirmed.entities().at(copies.at("room-area").id).properties.at("deduction_ids") ==
            std::vector<std::string>{copies.at("void-area").id} &&
        root_copy.properties.at("appraisal_facts").at("ansi").at("ceiling").at("below_5ft_deduction_ids").empty(),
        "actual sloped Facts Save retains both general-exclusion links and empty low-height sources");
    atomic_history(window,pasted,confirmed);
    persist_and_report(window,directory.path(),"sloped-nested-exclusions","1378");
}

void identity_only_sloped_leaf() {
    for (const int version : {1, 2}) {
        auto entities = fixture_entities();
        entities.erase(std::remove_if(entities.begin(), entities.end(), [](const auto& entity) {
            return entity.id == "floor-area" || entity.id == "void-area";
        }), entities.end());
        auto& room = find(entities, "room-area"); room.properties.erase("deduction_ids");
        const auto geometry = boundary_geometry(decode_identified_boundary_entity(room));
        Json ceiling{{"kind", "sloped"}, {"at_least_7ft_area_m2", 20}, {"room_floor_area_m2", 36},
            {"room_boundary_id", room.id}, {"below_5ft_deduction_ids", Json::array()},
            {"source_geometry_sha256", appraisal_ceiling_geometry_digest(geometry, {})}};
        if (version == 2) ceiling["complete_room_observed"] = true;
        room.properties["appraisal_facts"]["ansi"]["ceiling"] = ceiling;
        find(entities, "p").properties["appraisal_policy"]["version"] = version;
        QTemporaryDir directory; MainWindow window(std::make_shared<Document>(Document::create(entities)), nullptr, directory.filePath("text-library.json")); prepare(window);
        const auto before = window.document().snapshot(); expect_total(before, 36);
        require(window.selectEntity("room-area") && window.transformSelectedBoundary("0", false, false, "0 m", "0 m", true),
            "identity-only sloped leaf clone is admitted as unconfirmed geometry");
        const auto after = window.document().snapshot(); originals_unchanged(before, after);
        const auto copied_id = window.selectedEntityId().toStdString();
        const auto& copied_ceiling = after.entities().at(copied_id).properties.at("appraisal_facts").at("ansi").at("ceiling");
        require(copied_ceiling.at("source_geometry_sha256") == ceiling.at("source_geometry_sha256") &&
            copied_ceiling.value("room_boundary_id", std::string{}).empty(), "equal geometry digest cannot preserve a copied observation's confirmation binding");
        require(version == 2 ? !copied_ceiling.value("complete_room_observed", false) : !copied_ceiling.contains("complete_room_observed"),
            "copy revokes existing complete-room confirmation without inventing a V1 flag");
        const auto report = build_appraisal_document_report(after, "p"); bool copied_unqualified = false, original_qualified = false;
        for (const auto& status : report.boundaries) {
            if (status.boundary_id == copied_id) copied_unqualified = !status.qualification.qualified;
            if (status.boundary_id == "room-area") original_qualified = status.qualification.qualified;
        }
        require(copied_unqualified && original_qualified, "identity-only copied leaf itself is unqualified while original observations remain qualified");
        atomic_history(window, before, after);
    }
}
std::shared_ptr<Document> destination(bool ansi) {
    auto anchor = stroke("destination-anchor", -2, -1);
    for (const auto& [key, id] : std::vector<std::pair<std::string, std::string>>{
            {"property_id", "destination-property"}, {"building_id", "destination-building"},
            {"floor_id", "destination-floor"}, {"layer_id", "destination-layer"}}) anchor.properties[key] = id;
    const Json destination_policy = ansi ? policy() : Json{{"policy_kind", "residential_declared"},
        {"version", 1}, {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"}};
    return std::make_shared<Document>(Document::create({
        {"destination-property", "property", {{"name", "Clipboard destination"}, {"calculation_workflow", "appraisal"},
            {"appraisal_policy", destination_policy}}, false},
        {"destination-building", "building", {{"property_id", "destination-property"}}, false},
        {"destination-floor", "floor", {{"building_id", "destination-building"},
            {"appraisal_facts", {{"grade", "above"}, {"ansi", {{"any_part_below_grade", false}}}}}}, false},
        {"destination-layer", "layer", {{"floor_id", "destination-floor"}, {"name", "Destination"}}, false}, anchor}));
}
void destination_policy_admission_and_stairs() {
    QTemporaryDir directory; MainWindow source(sourced_fixture(), nullptr, directory.filePath("source-library.json")); prepare(source);
    require(source.selectEntity("floor-area") && source.copySelection(), "source policy validates during actual Copy");
    for (const bool ansi : {false, true}) {
        MainWindow target(destination(ansi), nullptr, directory.filePath(ansi ? "ansi-library.json" : "legacy-library.json")); prepare(target);
        require(target.selectEntity("destination-anchor"), "select actual destination drawing context");
        const auto before = target.document().snapshot();
        if (!ansi) {
            require(!target.pasteSelection() && target.document().snapshot().entities() == before.entities() &&
                target.document().revision() == before.revision() && target.document().snapshot().history().size() == before.history().size(),
                "nonANSI destination refuses nested paste atomically");
        } else {
            require(target.pasteSelection(), "different-project ANSI destination admits independently copied nested graph");
            const auto pasted = target.document().snapshot(); originals_unchanged(before, pasted);
            const auto copied = copied_areas(before, pasted);
            for (const auto& [original_id, owner] : copied) {
                require(owner.properties.at("floor_id") == "destination-floor" && owner.properties.at("layer_id") == "destination-layer",
                    "crossproject copy belongs to the actual destination floor and layer");
                require(owner.properties.at("appraisal_facts") == source.document().snapshot().entities().at(original_id).properties.at("appraisal_facts"),
                    "destination admission preserves raw source observations without inventing destination facts");
            }
            const auto checks = measurement_linework_source_checks(pasted.entities());
            for (const auto& [original_id, owner] : copied) require(checks.at(owner.id).current, "crossproject copied source lineage is current");
            expect_total(pasted, 96, "destination-property"); atomic_history(target, before, pasted);
        }
    }
    // A policy change after Copy is evaluated against the actual destination at Paste.
    MainWindow changed(destination(true), nullptr, directory.filePath("changed-library.json")); prepare(changed);
    require(changed.selectEntity("destination-anchor"), "select destination before policy change");
    auto property = changed.document().snapshot().entities().at("destination-property");
    property.properties["appraisal_policy"] = {{"policy_kind", "residential_declared"}, {"version", 1},
        {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"}};
    changed.document().apply(ApplyEntityChanges{changed.document().revision(), {EntityChange::upsert(property)}, {}, "Destination policy changed after copy"});
    const auto changed_before = changed.document().snapshot();
    require(!changed.pasteSelection() && changed.document().snapshot().entities() == changed_before.entities() &&
        changed.document().revision() == changed_before.revision(), "changed destination policy refuses captured nested clipboard graph without mutation");
    for (const bool valid : {true, false}) {
        auto entities = fixture_entities();
        entities.erase(std::remove_if(entities.begin(), entities.end(), [](const auto& entity) {
            return entity.id == "floor-area" || entity.id == "void-area";
        }), entities.end());
        auto& room = find(entities, "room-area"); room.properties.erase("deduction_ids");
        room.properties["appraisal_facts"]["boundary_role"] = "stair_footprint";
        room.properties["appraisal_facts"]["ansi"]["ceiling"] = {{"kind", "stairs"},
            {"stair_from_floor_id", valid ? "f" : "destination-floor"}};
        // Keep a real wrong source-floor identity whose name coincides with the destination.
        if (!valid) entities.push_back({"destination-floor", "floor", {{"building_id", "b"}}, false});
        MainWindow stairs(std::make_shared<Document>(Document::create(entities)), nullptr,
            directory.filePath(valid ? "valid-stairs.json" : "invalid-stairs.json")); prepare(stairs);
        if (valid) expect_total(stairs.document().snapshot(), 36);
        require(stairs.selectEntity("room-area") && stairs.copySelection(), "actual Copy preserves valid or unqualified recorded stair facts");
        MainWindow target(destination(true), nullptr, directory.filePath(valid ? "valid-stair-target.json" : "invalid-stair-target.json")); prepare(target);
        require(target.selectEntity("destination-anchor"), "select stair destination context"); const auto before = target.document().snapshot();
        require(target.pasteSelection(), "recorded stair geometry pastes independently"); const auto pasted = target.document().snapshot();
        const auto pasted_id = target.selectedEntityId().toStdString();
        const auto& binding = pasted.entities().at(pasted_id).properties.at("appraisal_facts").at("ansi").at("ceiling");
        if (valid) {
            require(binding.at("stair_from_floor_id") == "destination-floor", "valid source stair-floor binding maps to actual destination floor");
            expect_total(pasted, 36, "destination-property");
        } else require(!build_appraisal_document_report(pasted, "destination-property").qualified,
            "invalid source stair binding cannot become qualified by coincidental destination identity");
        atomic_history(target, before, pasted);
    }
}
void mixed_layers_and_invalid_sources() {
    QTemporaryDir directory; auto mixed = fixture_entities(); find(mixed, "room-area").properties.erase("layer_id");
    MainWindow window(std::make_shared<Document>(Document::create(mixed)), nullptr, directory.filePath("mixed-library.json")); prepare(window);
    const auto before = window.document().snapshot(); expect_total(before, 96);
    require(window.selectEntity("floor-area") && window.transformSelectedBoundary("0", false, false, "20 m", "0 m", true),
        "mixed layered and layerless same-floor ANSI nested graph clones safely");
    const auto after = window.document().snapshot(); originals_unchanged(before, after); (void)copied_areas(before, after);
    expect_total(after, 192); atomic_history(window, before, after);
    for (const auto* failure : {"legacy", "future-policy", "cycle", "site-grandchild", "stale-grandchild"}) {
        auto invalid = fixture_entities();
        if (std::string(failure) == "legacy") find(invalid, "p").properties["appraisal_policy"] = {
            {"policy_kind", "residential_declared"}, {"version", 1}, {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"}};
        else if (std::string(failure) == "future-policy") find(invalid, "p").properties["appraisal_policy"]["version"] = 99;
        else if (std::string(failure) == "cycle") find(invalid, "void-area").properties["deduction_ids"] = {"floor-area"};
        else if (std::string(failure) == "site-grandchild") find(invalid, "void-area").properties["calculation_scope"] = "site";
        else {
            const auto snapshot = sourced_fixture()->snapshot(); invalid.clear();
            for (const auto& [id, entity] : snapshot.entities()) invalid.push_back(entity);
            find(invalid, "void-area-source") = stroke("void-area-source", 5, 7);
        }
        MainWindow refused(std::make_shared<Document>(Document::create(invalid)), nullptr,
            directory.filePath(QString::fromLatin1(failure) + ".json")); prepare(refused);
        require(refused.selectEntity("floor-area"), "select actual invalid nested copy source");
        const auto retained = refused.document().snapshot(); const auto clipboard = QApplication::clipboard()->text();
        require(!refused.copySelection() && QApplication::clipboard()->text() == clipboard,
            "invalid source policy cycle or stale deep lineage refuses actual Copy without replacing clipboard");
        require(!refused.transformSelectedBoundary("0", false, false, "20 m", "0 m", true) &&
            refused.document().snapshot().entities() == retained.entities() && refused.document().revision() == retained.revision() &&
            refused.document().snapshot().history().size() == retained.history().size(),
            "invalid nested clone refuses atomically with exact original document and history");
    }
}
void site_leaf_and_mixed_selection() {
    QTemporaryDir directory; auto entities = fixture_entities();
    auto site = area("site-area", 12, 14); site.type = "boundary";
    site.properties["calculation_scope"] = "site"; site.properties["classification"] = "survey";
    site.properties.erase("appraisal_category"); site.properties.erase("appraisal_facts"); entities.push_back(site);
    MainWindow window(std::make_shared<Document>(Document::create(entities)), nullptr, directory.filePath("site-library.json")); prepare(window);
    const auto before = window.document().snapshot(); expect_total(before, 96);
    require(window.selectEntity("site-area") && window.transformSelectedBoundary("0", false, false, "20 m", "0 m", true),
        "site survey leaf still clones in an ANSI project");
    const auto cloned = window.document().snapshot(); originals_unchanged(before, cloned); expect_total(cloned, 96);
    const auto copied_site = cloned.entities().at(window.selectedEntityId().toStdString());
    require(copied_site.properties.at("calculation_scope") == "site" && copied_site.extensions.at("vendor") == site.extensions.at("vendor"),
        "site leaf copy retains its independent site scope and metadata");
    atomic_history(window, before, cloned); require(window.undoCommand(), "remove leaf clone before mixed selection");
    require(window.selectEntity("floor-area") && window.selectEntity("site-area", true) && window.copySelection(),
        "actual mixed selection copies ANSI building graph and ordinary site leaf together");
    const auto mixed_before = window.document().snapshot(); require(window.pasteSelection(), "actual mixed graph paste admits each root under its own area scope");
    const auto pasted = window.document().snapshot(); originals_unchanged(mixed_before, pasted);
    int building_owners = 0, site_owners = 0;
    for (const auto& [id, entity] : pasted.entities()) if (!mixed_before.entities().contains(id)) {
        if (entity.type == "measurement_boundary") ++building_owners;
        if (entity.type == "boundary" && entity.properties.value("calculation_scope", std::string{}) == "site") ++site_owners;
    }
    require(building_owners == 3 && site_owners == 1, "mixed clipboard graph retains complete nested building graph and one site leaf");
    atomic_history(window, mixed_before, pasted);
}
void native_nested_clone() {
    QTemporaryDir directory; require(directory.isValid(), "isolated nested copy fixture");
    MainWindow window(fixture(), nullptr, directory.filePath("text-library.json")); prepare(window);
    const auto before = window.document().snapshot(); expect_total(before, 96);
    require(window.selectEntity("floor-area"), "select real nested ANSI floor owner");
    if (!window.transformSelectedBoundary("0", false, false, "20 m", "0 m", true))
        throw std::runtime_error("Native nested ANSI clone must copy the complete floor-room-void graph: " + window.lastError().toStdString());
    const auto after = window.document().snapshot(); expect_total(after, 192);
    require(after.revision() == before.revision() + 1 && after.history().size() == before.history().size() + 1,
        "nested clone is one atomic history command");
    for (const auto& [id, original] : before.entities())
        require(after.entities().at(id) == original, "nested clone preserves every original entity exactly");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == after.entities(),
        "one Undo and Redo restore the complete nested clone exactly");
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-nested-appraisal-copy-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0, "bundled font loads");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        native_nested_clone(); flat_graph_clone_clipboard_and_dialog(); sloped_graph_requires_native_reconfirmation();
        sloped_root_retains_nested_general_exclusions();
        identity_only_sloped_leaf(); destination_policy_admission_and_stairs(); mixed_layers_and_invalid_sources();
        site_leaf_and_mixed_selection();
    } catch (const std::exception& error) { std::cerr << "nested_appraisal_copy_desktop_tests: " << error.what() << '\n'; return 1; }
    std::cout << "Nested appraisal copy desktop tests passed\n"; return 0;
}
