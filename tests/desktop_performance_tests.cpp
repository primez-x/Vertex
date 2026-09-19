#include "sketch/desktop/main_window.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/visualization/native_model_view.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QTemporaryDir>

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
    int failures = 0;
    for (const auto test : {actual_sheet_counts_and_unknown_measurements,
                           refresh_is_not_an_edit_and_navigation_waits_for_paint,
                           replacing_document_partitions_samples,
                           readiness_requires_actual_geometry}) {
        try { test(); }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
