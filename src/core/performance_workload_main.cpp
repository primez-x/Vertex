#include "sketch/performance_workload.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include <Standard_Version.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::invalid_argument(
            "usage: performance-workload <drawing|architecture|sheets> <new.bldproj> "
            "<developer-hardware-label> <source-revision-label>; JSON report goes to stdout");
        const std::string kind = argv[1];
        sketch::PerformanceWorkloadOptions options;
        if (kind == "drawing") options = {sketch::PerformanceWorkloadKind::drawing, 50000, 0};
        else if (kind == "architecture") options = {sketch::PerformanceWorkloadKind::architecture, 10000, 0};
        else if (kind == "sheets") options = {sketch::PerformanceWorkloadKind::sheets, 20, 12500000};
        else throw std::invalid_argument("unknown workload kind");
        if (std::string(argv[3]).find_first_not_of(" \t\r\n") == std::string::npos ||
            std::string(argv[4]).find_first_not_of(" \t\r\n") == std::string::npos)
            throw std::invalid_argument("hardware and revision labels must be nonblank");
        const std::filesystem::path path = argv[2];
        auto roundtrip_path = path;
        roundtrip_path += ".roundtrip.bldproj";
        // symlink_status also catches dangling links. ProjectStore's exclusive
        // save remains the race-safe authority after this early preflight.
        if (std::filesystem::exists(std::filesystem::symlink_status(path)) ||
            std::filesystem::exists(std::filesystem::symlink_status(roundtrip_path)))
            throw std::invalid_argument("workload destinations must both be new paths");
        auto document = sketch::make_performance_workload(options);
        const auto original = document.snapshot();
        const auto facts = sketch::inspect_performance_workload(original);
        const auto source_digest = sketch::document_authoring_source_digest_v1(original);
        auto started = Clock::now();
        const auto saved = sketch::ProjectStore::save(path, original);
        const auto save_ms = elapsed(started);
        started = Clock::now();
        auto loaded = sketch::ProjectStore::load(path);
        const auto open_ms = elapsed(started);
        const auto reopened = loaded.document.snapshot();
        if (loaded.file_sha256 != saved.file_sha256 ||
            source_digest != sketch::document_authoring_source_digest_v1(reopened) ||
            facts != sketch::inspect_performance_workload(reopened))
            throw std::runtime_error("initial save/open integrity comparison failed");
        started = Clock::now();
        const auto resaved = sketch::ProjectStore::save(roundtrip_path, reopened);
        const auto resave_ms = elapsed(started);
        started = Clock::now();
        auto reloaded = sketch::ProjectStore::load(roundtrip_path);
        const auto reopen_ms = elapsed(started);
        if (resaved.file_sha256 != reloaded.file_sha256 ||
            source_digest != sketch::document_authoring_source_digest_v1(reloaded.document.snapshot()) ||
            facts != sketch::inspect_performance_workload(reloaded.document.snapshot()))
            throw std::runtime_error("second save/open integrity comparison failed");
        auto workload = facts;
        workload["id"] = "vertex-representative-" + kind + "-v1";
        workload["project_bytes"] = std::filesystem::file_size(path);
        const nlohmann::json report = {
            {"schema_version", 1}, {"audit_status", "incomplete"},
            {"generator", "vertex-representative-workload-v1"}, {"workload", workload},
            {"provenance", {{"reference_hardware", argv[3]}, {"source_revision", argv[4]},
                {"labels_verified", false}, {"occt_version", OCC_VERSION_COMPLETE},
                {"measurement_scope", "developer-host synchronous ProjectStore API; no GUI timing"},
                {"semantic_determinism", "entity and asset content; fresh random document identity"}}},
            {"integrity", {{"authoring_source_sha256", source_digest},
                {"document_id", original.document_id()}, {"revision", original.revision()},
                {"snapshot_and_assets_equal_after_both_reopens", true},
                {"project_sha256", saved.file_sha256}, {"roundtrip_sha256", resaved.file_sha256}}},
            {"samples_ms", {{"save", {save_ms, resave_ms}}, {"open", {open_ms, reopen_ms}}}},
            {"unresolved", {"agreed reference hardware", "interactive navigation/input/edit timings",
                "statistical adequacy", "production performance qualification",
                "sheet image decoding and rendered output timing are not measured by this CLI"}}};
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "performance-workload: " << error.what() << '\n';
        return 2;
    }
}
