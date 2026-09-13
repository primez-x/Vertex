#include "sketch/project_resource_catalog.hpp"
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using sketch::ProjectResourceCatalog;
using sketch::ProjectResourceKind;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "project_resource_catalog_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

std::filesystem::path temporary_root() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("property-studio-resource-catalog-" +
                       std::to_string(std::rand()));
    std::filesystem::create_directories(root);
    return root;
}

void write_bytes(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(output.good(), "resource fixture could not be written");
}

void write_manifest(const std::filesystem::path& root, const nlohmann::json& resources,
                    const nlohmann::json& files) {
    const nlohmann::json manifest{
        {"schema_version", 1}, {"manifest_version", 1},
        {"manifest_kind", "project-package"}, {"audit_status", "incomplete"},
        {"offline_qualified", false}, {"resources", resources}, {"files", files},
        {"summary", {{"file_count", files.size() + 1},
                      {"asset_count", 0}, {"template_count", 1},
                      {"profile_count", 1}, {"documentation_count", 1}}}};
    std::ofstream output(root / "project-package-manifest.json", std::ios::binary);
    output << manifest.dump(2) << '\n';
    require(output.good(), "package manifest could not be written");
}

void valid_package_fixture(const std::filesystem::path& root) {
    write_bytes(root / "templates/residential.json",
                "{\"kind\":\"template\",\"name\":\"Residential\"}\n");
    write_bytes(root / "profiles/imperial.json", "{\"units\":\"imperial\"}\n");
    write_bytes(root / "documentation/project-format.md", "Project format reference\n");
    const nlohmann::json resources = nlohmann::json::array({
        {{"kind", "template"}, {"name", "Residential"},
         {"path", "templates/residential.json"},
         {"sha256", "4c3fedb6570dc96b5c324117c205671d1adde3bc1df1a50d1120fa31d0cc8dd4"},
         {"size", 41}},
        {{"kind", "profile"}, {"name", "Imperial"},
         {"path", "profiles/imperial.json"},
         {"sha256", "b8e2dadb98ccf65acdfb3713ad3cc551e28711ae953b13d4d56a723cda8e2ca4"},
         {"size", 21}},
        {{"kind", "documentation"}, {"name", "Project format"},
         {"path", "documentation/project-format.md"},
         {"sha256", "d411a467061e10e13d1668d8b39e9e4a1e3c350fb2ca379669297b2869e5fcb0"},
         {"size", 25}},
    });
    const nlohmann::json files = nlohmann::json::array({
        {{"kind", "documentation"}, {"path", "documentation/project-format.md"},
         {"sha256", "d411a467061e10e13d1668d8b39e9e4a1e3c350fb2ca379669297b2869e5fcb0"},
         {"size", 25}},
        {{"kind", "profile"}, {"path", "profiles/imperial.json"},
         {"sha256", "b8e2dadb98ccf65acdfb3713ad3cc551e28711ae953b13d4d56a723cda8e2ca4"},
         {"size", 21}},
        {{"kind", "template"}, {"path", "templates/residential.json"},
         {"sha256", "4c3fedb6570dc96b5c324117c205671d1adde3bc1df1a50d1120fa31d0cc8dd4"},
         {"size", 41}},
    });
    write_manifest(root, resources, files);
}

void reads_registered_resources() {
    const auto root = temporary_root();
    valid_package_fixture(root);
    const auto cleanup = [&] { std::filesystem::remove_all(root); };
    try {
        const auto catalog = ProjectResourceCatalog::register_package(root);
        require(catalog.resources().size() == 3, "all declared resources should register");
        require(catalog.resources(ProjectResourceKind::template_resource).size() == 1,
                "template filter should be deterministic");
        const auto template_resource = catalog.find(ProjectResourceKind::template_resource,
                                                     "Residential");
        require(template_resource.has_value(), "template should be discoverable by name");
        require(template_resource->relative_path.generic_string() == "templates/residential.json",
                "template path should remain package-relative");
        const auto bytes = catalog.read(ProjectResourceKind::documentation, "Project format");
        require(std::string(bytes.begin(), bytes.end()) == "Project format reference\n",
                "documentation bytes should be read from the verified package");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

void rejects_modified_payload() {
    const auto root = temporary_root();
    valid_package_fixture(root);
    write_bytes(root / "profiles/imperial.json", "{\"units\":\"metric\"}\n");
    bool rejected = false;
    try {
        (void)ProjectResourceCatalog::register_package(root);
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("hash") != std::string::npos;
    }
    std::filesystem::remove_all(root);
    require(rejected, "modified resource bytes must be rejected by hash");
}

void rejects_unsafe_or_ambiguous_manifest() {
    const auto root = temporary_root();
    valid_package_fixture(root);
    auto manifest = nlohmann::json::parse(
        std::ifstream(root / "project-package-manifest.json", std::ios::binary), nullptr, true, true);
    manifest["resources"][0]["path"] = "../outside.json";
    std::ofstream(root / "project-package-manifest.json", std::ios::binary)
        << manifest.dump(2) << '\n';
    bool traversal_rejected = false;
    try { (void)ProjectResourceCatalog::register_package(root); }
    catch (const std::invalid_argument&) { traversal_rejected = true; }
    require(traversal_rejected, "resource traversal must be rejected");

    valid_package_fixture(root);
    manifest = nlohmann::json::parse(
        std::ifstream(root / "project-package-manifest.json", std::ios::binary), nullptr, true, true);
    manifest["resources"][1]["kind"] = "template";
    manifest["resources"][1]["name"] = "Residential";
    std::ofstream(root / "project-package-manifest.json", std::ios::binary)
        << manifest.dump(2) << '\n';
    bool duplicate_rejected = false;
    try { (void)ProjectResourceCatalog::register_package(root); }
    catch (const std::invalid_argument&) { duplicate_rejected = true; }
    std::filesystem::remove_all(root);
    require(duplicate_rejected, "duplicate resource names must be rejected");
}

void rejects_invalid_structured_resource() {
    const auto root = temporary_root();
    valid_package_fixture(root);
    write_bytes(root / "templates/residential.json", "not-json\n");
    auto manifest = nlohmann::json::parse(
        std::ifstream(root / "project-package-manifest.json", std::ios::binary), nullptr, true, true);
    manifest["resources"][0]["sha256"] =
        "60498ebafa3f473a2a72c1242e8c3202bf50a6d81dfc721958be1550f46faf33";
    manifest["resources"][0]["size"] = 9;
    manifest["files"][2]["sha256"] = manifest["resources"][0]["sha256"];
    manifest["files"][2]["size"] = 9;
    std::ofstream(root / "project-package-manifest.json", std::ios::binary)
        << manifest.dump(2) << '\n';
    bool rejected = false;
    try { (void)ProjectResourceCatalog::register_package(root); }
    catch (const std::invalid_argument&) { rejected = true; }
    std::filesystem::remove_all(root);
    require(rejected, "template resources must contain valid JSON");
}

}  // namespace

int main() {
    reads_registered_resources();
    rejects_modified_payload();
    rejects_unsafe_or_ambiguous_manifest();
    rejects_invalid_structured_resource();
    std::cout << "project_resource_catalog_tests passed\n";
}
