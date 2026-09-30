#include "sketch/project_import_worker.hpp"
#include "support/noninteractive_errors.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <windows.h>

namespace {
void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}
sketch::ProjectImportCandidate run(const std::filesystem::path& root,
    const std::filesystem::path& fixture, sketch::ProjectImportKind kind) {
    std::ifstream input(fixture, std::ios::binary);
    require(input.good(), "Missing generated CAD fixture");
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    sketch::WindowsImportWorkerOptions options;
    options.executable = root / "vertex-import-worker.exe";
    options.immutable_module_roots = {root};
    options.temporary_root = std::filesystem::temp_directory_path();
    auto result = sketch::import_project_in_worker(
        std::as_bytes(std::span(bytes.data(), bytes.size())), kind, std::move(options),
        [](const auto& request) {
            auto report = sketch::run_windows_import_worker(request);
            if (!report.controls_attested()) {
                std::cerr << report.to_json().dump() << '\n';
            }
            return report;
        });
    require(result.isolation_controls_attested && result.source_retention_required,
        "Library import must attest sandbox controls and retain original source");
    return result;
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        wchar_t executable[32768];
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        require(length > 0 && length < 32768, "Cannot locate test runtime");
        auto root = std::filesystem::path(std::wstring(executable, length)).parent_path();
        wchar_t installed_root[32768];
        const auto installed_length = GetEnvironmentVariableW(
            L"VERTEX_TEST_RUNTIME_ROOT", installed_root, 32768);
        if (installed_length) {
            require(installed_length < 32768, "Installed runtime path exceeds test limit");
            root = std::filesystem::canonical(std::filesystem::path(
                std::wstring(installed_root, installed_length)));
            require(std::filesystem::is_regular_file(root / "vertex-import-worker.exe"),
                "Installed runtime does not contain the import worker");
        }
        const auto* capture = std::getenv("VERTEX_TEST_CAPTURE_DIR");
        require(capture != nullptr, "Use independent worker runner for immutable fixtures");
        const auto fixtures = std::filesystem::path(capture) / "cad-fixtures";
        {
            sketch::WindowsImportWorkerOptions probe;
            probe.executable = root / "vertex-import-worker.exe";
            probe.immutable_module_roots = {root};
            probe.temporary_root = std::filesystem::temp_directory_path();
            probe.arguments = {L"dxf", L"0"};
            const std::string malformed = "VERTEX_ENTITY_V1";
            probe.input.assign(reinterpret_cast<const std::byte*>(malformed.data()),
                reinterpret_cast<const std::byte*>(malformed.data() + malformed.size()));
            const auto report = sketch::run_windows_import_worker(probe);
            if (!report.launched || report.exit_code != 4) std::cerr << report.to_json().dump() << '\n';
            require(report.launched && report.exit_code == 4, "Worker startup/native-only rejection failed");
        }
        const auto dxf = run(root, fixtures / "nested-binary.dxf", sketch::ProjectImportKind::dxf);
        require(dxf.entities.size() == 1 && dxf.entities.front().type == "boundary",
            "Binary nested block must import one world-space outline");
        const auto& segment = dxf.entities.front().properties.at("boundary").at(0);
        require(std::abs(segment.at("start")[0].get<double>() - 12) < 1e-7 &&
                std::abs(segment.at("end")[0].get<double>() - 13) < 1e-7 &&
                std::abs(segment.at("start")[1].get<double>() - 20) < 1e-7,
            "Nested block transforms or source units were lost");
        const auto hollow = run(root, fixtures / "hollow-rotated.ifc", sketch::ProjectImportKind::ifc);
        std::size_t outers = 0, holes = 0;
        for (const auto& entity : hollow.entities) {
            if (!entity.extensions.contains("ifc_library_section")) continue;
            const auto role = entity.extensions.at("ifc_library_section").at("role");
            outers += role == "outer"; holes += role == "hole";
            require(entity.type == "boundary", "Foreign sections must not invent architectural semantics");
        }
        require(outers == 1 && holes == 1, "IFC section must preserve the inner void");
        const auto disconnected = run(root, fixtures / "disconnected.ifc", sketch::ProjectImportKind::ifc);
        std::size_t components = 0;
        for (const auto& entity : disconnected.entities)
            components += entity.extensions.contains("ifc_library_section");
        require(components == 2, "IFC section must preserve disconnected components");
        const auto slab = run(root, fixtures / "slab-void.ifc", sketch::ProjectImportKind::ifc);
        std::size_t slab_outers = 0, slab_holes = 0, typed_slabs = 0;
        for (const auto& entity : slab.entities) {
            typed_slabs += entity.type == "slab";
            if (!entity.extensions.contains("ifc_library_section")) continue;
            const auto& section = entity.extensions.at("ifc_library_section");
            if (section.at("source_kind") != "IfcSlab") continue;
            slab_outers += section.at("role") == "outer";
            slab_holes += section.at("role") == "hole";
        }
        require(typed_slabs == 1 && slab_outers == 1 && slab_holes == 1,
            "Foreign FLOOR slab must retain its typed entity and void-aware measurement contours");
        const auto ambiguous = run(root, fixtures / "units-unresolved.ifc", sketch::ProjectImportKind::ifc);
        require(std::all_of(ambiguous.entities.begin(), ambiguous.entities.end(),
            [](const auto& entity) { return entity.type == "ifc_reference"; }) &&
            std::any_of(ambiguous.diagnostics.begin(), ambiguous.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "ifc_length_units_unresolved"; }),
            "Unitless IFC must retain its source without inventing physical dimensions");
        std::cout << "CAD library AppContainer imports passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
