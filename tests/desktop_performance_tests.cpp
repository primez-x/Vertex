#include "sketch/desktop/main_window.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/visualization/native_model_view.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QTemporaryDir>
#include <QFile>

#include <filesystem>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using json = nlohmann::json;
using sketch::desktop::MainWindow;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

json report(const MainWindow& window) {
    return json::parse(window.performanceReportJson().toStdString());
}

// Drives the same command/event/paint paths as the desktop. This deliberately
// does not inject durations or broaden the application's measurement boundary.
json capture_interactions(MainWindow& window, int samples) {
    require(samples > 0 && samples <= 1000 && samples % 2 == 0,
            "capture count must be an even integer from 2 through 1000");
    const auto original = window.document().snapshot();
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    sketch::desktop::PlanCanvas* canvas = nullptr;
    for (auto* widget : window.findChildren<QWidget*>()) {
        auto* candidate = dynamic_cast<sketch::desktop::PlanCanvas*>(widget);
        if (candidate && candidate->isVisible()) { canvas = candidate; break; }
    }
    require(canvas != nullptr, "capture requires a visible plan canvas");
    // Representative core fixtures have no desktop drawing-layer records.
    // Prepare a normal document history entry directly, outside timed capture;
    // all measured undo/redo operations still use the desktop command path.
    const auto probe_id = sketch::make_stable_id();
    sketch::IdentifiedBoundary probe{probe_id, "measurement_boundary", {
        {probe_id + "s0", probe_id + "v0", probe_id + "v1", {{0, 0}, {5, 0}, 0}},
        {probe_id + "s1", probe_id + "v1", probe_id + "v2", {{5, 0}, {0, 4}, 0}},
        {probe_id + "s2", probe_id + "v2", probe_id + "v0", {{0, 4}, {0, 0}, 0}}}};
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(sketch::encode_identified_boundary_entity(probe))},
        .message = "prepare temporary performance capture probe"});
    if (!window.undoCommand())
        throw std::runtime_error("capture probe setup failed: " + window.lastError().toStdString());
    QApplication::processEvents();
    window.beginPerformanceRun();
    const auto wait_for_sample = [&](const char* metric, int expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            QApplication::processEvents();
            const auto count = report(window).at("metrics").at(metric).at("sample_count").get<int>();
            require(count <= expected, "unexpected extra metric sample during capture");
            if (count == expected) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error(std::string("paint sample timeout: ") + metric);
    };
    for (int index = 0; index < samples; ++index) {
        canvas->zoomBy(index % 2 == 0 ? 1.01 : 1.0 / 1.01);
        wait_for_sample("navigation", index + 1);
        const QPointF position(100 + index % 50, 100 + index % 37);
        QMouseEvent event(QEvent::MouseMove, position, position, Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
        wait_for_sample("input", index + 1);
        require(index % 2 == 0 ? window.redoCommand() : window.undoCommand(),
                "capture history command failed");
        wait_for_sample("edit", index + 1);
        if (samples >= 100 && (index + 1) % 10 == 0)
            std::cerr << "interaction capture: " << index + 1 << '/' << samples
                      << " navigation/input/edit triplets completed\n";
    }
    const auto after = window.document().snapshot();
    require(original.document_id() == after.document_id() &&
                original.entities() == after.entities() && original.assets() == after.assets(),
            "capture failed to restore original entities and asset bytes");
    auto result = json{
        {"schema", "vertex-desktop-interaction-capture"}, {"schema_version", 1},
        {"audit_status", "incomplete"}, {"desktop_report", report(window)},
        {"protocol", "offscreen 1200x800 plan canvas; alternating zoom; pointer move; probe boundary redo/undo"},
        {"requested_samples_per_interactive_metric", samples},
        {"integrity", {{"document_id", after.document_id()},
            {"revision_before", original.revision()}, {"revision_after", after.revision()},
            {"entities_and_asset_bytes_restored", true},
            {"entities_sha256", sketch::entity_map_digest(after.entities())}}},
        {"unresolved", {"native 3D and compositor/display timings", "sheet image decoding",
            "reference hardware agreement", "desktop open/save sample collection",
            "long-regeneration cancellation observation", "production qualification"}}};
    window.document().mark_saved(window.document().revision());
    return result;
}

void bounded_capture_uses_real_paint_and_preserves_content() {
    // Core representative workloads intentionally have no desktop layers.
    MainWindow window(std::make_shared<sketch::Document>(sketch::Document::create()));
    for (const int invalid : {0, 1, 1002}) {
        bool rejected = false;
        try { (void)capture_interactions(window, invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "capture must reject unbounded or unpaired sample requests");
    }
    const auto result = capture_interactions(window, 2);
    for (const auto* name : {"navigation", "input", "edit"}) {
        const auto& metric = result.at("desktop_report").at("metrics").at(name);
        require(metric.at("sample_count") == 2 && metric.at("dropped_sample_count") == 0,
                "capture must retain exactly one real sample per requested operation");
    }
    require(result.at("desktop_report").at("metrics").at("open").at("sample_count") == 0,
            "interaction capture must not fabricate storage observations");
}

void actual_sheet_counts_and_unknown_measurements() {
    auto document = std::make_shared<sketch::Document>(sketch::Document::create());
    std::vector<sketch::DrawingSheet> sheets;
    for (int index = 0; index < 20; ++index) {
        sketch::DrawingSheet sheet;
        sheet.id = "sheet-" + std::to_string(index);
        sheet.number = "A-" + std::to_string(index);
        sheets.push_back(std::move(sheet));
    }
    const auto model = sketch::SheetViewModel::create({}, std::move(sheets));
    document->apply(sketch::ApplyEntityChanges{
        .expected_revision = document->revision(),
        .entity_changes = {sketch::EntityChange::upsert(
            sketch::make_sheet_view_entity("sheets", model))},
        .message = "create sheet workload",
    });
    MainWindow window(document);
    const auto result = report(window);
    require(result.at("workload").at("sheets") == 20,
            "report must count decoded sheets, not sheet-model containers");
    require(result.at("workload").at("triangles").is_null(),
            "unmeasured tessellation must be unknown, not measured zero");
    require(result.at("workload").at("project_bytes").is_null(),
            "unsaved file size must be unknown, not measured zero");
    require(result.at("audit_status") == "incomplete",
            "local instrumentation cannot certify production acceptance");
}

void refresh_is_not_an_edit_and_navigation_waits_for_paint() {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    const auto before = report(window);
    require(before.at("metrics").at("edit").at("sample_count") == 0,
            "constructing or refreshing a workspace is not a document edit");
    const auto navigation_before = before.at("metrics").at("navigation").at("sample_count");
    window.fitView();
    require(report(window).at("metrics").at("navigation").at("sample_count") == navigation_before,
            "fit-view handler must not be reported as completed navigation");
    QApplication::processEvents();
    require(report(window).at("metrics").at("navigation").at("sample_count") > navigation_before,
            "navigation must be measured through a completed canvas paint");
    const auto id = window.createBoundary({
        {{0, 0}, {5, 0}, 0}, {{5, 0}, {5, 4}, 0},
        {{5, 4}, {0, 4}, 0}, {{0, 4}, {0, 0}, 0}});
    require(!id.isEmpty(), "performance fixture boundary creation failed");
    require(report(window).at("metrics").at("edit").at("sample_count") == 0,
            "document commit must wait for visual completion before recording edit latency");
    QApplication::processEvents();
    const auto edits = report(window).at("metrics").at("edit").at("sample_count");
    require(edits == 1, "one committed command and paint must produce one edit sample");
    require(window.selectEntity(id), "fixture selection failed");
    QApplication::processEvents();
    require(report(window).at("metrics").at("edit").at("sample_count") == edits,
            "selection refresh must not contaminate edit timing");
    window.document().mark_saved(window.document().revision());
}

void replacing_document_partitions_samples() {
    MainWindow window;
    QTemporaryDir directory;
    require(directory.isValid(), "performance fixture needs temporary storage");
    const auto path = directory.filePath(QStringLiteral("performance.bldproj"));
    require(window.saveProjectAs(path), "performance fixture save failed");
    const auto saved = report(window);
    require(saved.at("metrics").at("save").at("sample_count") == 1,
            "successful save must be measured");
    require(saved.at("workload").at("project_bytes") ==
                std::filesystem::file_size(std::filesystem::path(path.toStdWString())),
            "report must use actual saved file bytes");
    require(window.createNewProject(), "clean new-project transition failed");
    const auto fresh = report(window);
    for (const auto& [name, metric] : fresh.at("metrics").items()) {
        (void)name;
        require(metric.at("sample_count") == 0,
                "a new document must not inherit samples from the previous document");
    }
    require(window.openProject(path), "performance fixture reopen failed");
    const auto reopened = report(window);
    require(reopened.at("metrics").at("open").at("sample_count") == 1 &&
                reopened.at("metrics").at("save").at("sample_count") == 0,
            "opened document must contain only its own operation samples");
    require(reopened.at("workload").at("document_id") == window.document().snapshot().document_id(),
            "workload metadata must identify the measured document");
}

void readiness_requires_actual_geometry() {
    for (const double height : {2.8, -1.0}) {
        auto document = std::make_shared<sketch::Document>(sketch::Document::create({
            sketch::Entity::create("wall", {
                {"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
                              {"sweep_radians", 0.0}}},
                {"thickness_m", 0.2}, {"height_m", height}, {"elevation_m", 0.0}})}));
        MainWindow window(document);
        sketch::visualization::NativeModelView* native = nullptr;
        for (auto* widget : window.findChildren<QWidget*>()) {
            if (auto* view = dynamic_cast<sketch::visualization::NativeModelView*>(widget)) {
                native = view;
                break;
            }
        }
        require(native != nullptr, "offscreen desktop must still prepare real semantic geometry");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool ready = false;
        do {
            ready = window.regenerationReadyForCurrentRevision();
            if (!native->isGeometryPending()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        require(!native->isGeometryPending(), "desktop geometry preparation timed out");
        require(ready == (height > 0.0),
                "desktop readiness must reject invalid wall geometry and accept prepared valid geometry");
        require(!native->isVisible(), "semantic preparation must not open a native window");
    }
}
}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--capture"))) {
        try {
            const auto args = application.arguments();
            require(args.size() == 4 && args[1] == QStringLiteral("--capture"),
                    "usage: desktop_performance_tests --capture input.bldproj new-output.json");
            QFile output(args[3]);
            require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                    "capture output must be a writable new path");
            auto loaded = sketch::ProjectStore::load(std::filesystem::path(args[2].toStdWString()));
            const auto source_digest = sketch::document_authoring_source_digest_v1(loaded.document.snapshot());
            MainWindow window(std::make_shared<sketch::Document>(std::move(loaded.document)));
            auto result = capture_interactions(window, 100);
            result["source_project_sha256"] = loaded.file_sha256;
            result["source_authoring_sha256"] = source_digest;
            const auto bytes = QByteArray::fromStdString(result.dump(2) + "\n");
            require(output.write(bytes) == bytes.size() && output.flush(), "capture output write failed");
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "interaction capture: " << error.what() << '\n';
            return 2;
        }
    }
    int failures = 0;
    for (const auto test : {actual_sheet_counts_and_unknown_measurements,
                           refresh_is_not_an_edit_and_navigation_waits_for_paint,
                           replacing_document_partitions_samples,
                           bounded_capture_uses_real_paint_and_preserves_content,
                           readiness_requires_actual_geometry}) {
        try { test(); }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
