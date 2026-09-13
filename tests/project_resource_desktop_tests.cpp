#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write_bytes(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(output.good(), "resource desktop fixture could not be written");
}

void write_fixture(const std::filesystem::path& root) {
    constexpr std::string_view template_bytes =
        "{\"kind\":\"template\",\"name\":\"Residential\"}\n";
    constexpr std::string_view profile_bytes = "{\"units\":\"imperial\"}\n";
    constexpr std::string_view documentation_bytes = "Project format reference\n";
    write_bytes(root / "templates/residential.json", template_bytes);
    write_bytes(root / "profiles/imperial.json", profile_bytes);
    write_bytes(root / "documentation/project-format.md", documentation_bytes);
    const json resources = json::array({
        {{"kind", "template"}, {"name", "Residential"},
         {"path", "templates/residential.json"},
         {"sha256", "4c3fedb6570dc96b5c324117c205671d1adde3bc1df1a50d1120fa31d0cc8dd4"},
         {"size", template_bytes.size()}},
        {{"kind", "profile"}, {"name", "Imperial"},
         {"path", "profiles/imperial.json"},
         {"sha256", "b8e2dadb98ccf65acdfb3713ad3cc551e28711ae953b13d4d56a723cda8e2ca4"},
         {"size", profile_bytes.size()}},
        {{"kind", "documentation"}, {"name", "Project format"},
         {"path", "documentation/project-format.md"},
         {"sha256", "d411a467061e10e13d1668d8b39e9e4a1e3c350fb2ca379669297b2869e5fcb0"},
         {"size", documentation_bytes.size()}},
    });
    const json files = json::array({
        {{"kind", "documentation"}, {"path", "documentation/project-format.md"},
         {"sha256", "d411a467061e10e13d1668d8b39e9e4a1e3c350fb2ca379669297b2869e5fcb0"},
         {"size", documentation_bytes.size()}},
        {{"kind", "profile"}, {"path", "profiles/imperial.json"},
         {"sha256", "b8e2dadb98ccf65acdfb3713ad3cc551e28711ae953b13d4d56a723cda8e2ca4"},
         {"size", profile_bytes.size()}},
        {{"kind", "template"}, {"path", "templates/residential.json"},
         {"sha256", "4c3fedb6570dc96b5c324117c205671d1adde3bc1df1a50d1120fa31d0cc8dd4"},
         {"size", template_bytes.size()}},
    });
    const json manifest{
        {"schema_version", 1}, {"manifest_version", 1},
        {"manifest_kind", "project-package"}, {"audit_status", "incomplete"},
        {"offline_qualified", false}, {"resources", resources}, {"files", files},
        {"summary", {{"file_count", 4}, {"asset_count", 0},
                      {"template_count", 1}, {"profile_count", 1},
                      {"documentation_count", 1}}}};
    std::ofstream output(root / "project-package-manifest.json", std::ios::binary);
    output << manifest.dump(2) << '\n';
    require(output.good(), "resource desktop manifest could not be written");
}

void exercise_desktop_registration() {
    using sketch::desktop::MainWindow;
    QTemporaryDir temporary;
    require(temporary.isValid(), "resource desktop fixture needs a temporary directory");
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    write_fixture(root);

    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    const auto revision = window.document().revision();
    require(window.registerProjectPackageResources(
                QString::fromStdWString(root.wstring())),
            window.lastError().toStdString());
    require(window.document().revision() == revision,
            "resource registration must not mutate the active document");
    require(window.registeredProjectResourceNames() ==
                QStringList{QStringLiteral("documentation: Project format"),
                            QStringLiteral("profile: Imperial"),
                            QStringLiteral("template: Residential")},
            "desktop registration should expose deterministic resource names");
    require(window.findChild<QAction*>(QStringLiteral("projectResources")) != nullptr,
            "project resources action must be available in the secondary command menu");

    write_bytes(root / "documentation/project-format.md", "changed after registration\n");
    require(!window.registerProjectPackageResources(
                QString::fromStdWString(root.wstring())),
            "desktop registration must reject modified package bytes");
    require(window.lastError().contains(QStringLiteral("hash")),
            "desktop registration should report the package hash failure");
    require(window.document().revision() == revision,
            "failed resource registration must not mutate the active document");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        exercise_desktop_registration();
        std::cout << "project_resource_desktop_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project_resource_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
