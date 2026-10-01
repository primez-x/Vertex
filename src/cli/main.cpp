#include "sketch/document.hpp"
#include "sketch/annotation_catalog.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_resource_catalog.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <algorithm>
#include <cwchar>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace {
using Json = nlohmann::json;
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::string utf8_text(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') return {};
    const auto length = static_cast<int>(wcslen(value));
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("could not convert command text to UTF-8");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, result.data(),
                            required, nullptr, nullptr) != required) {
        throw std::runtime_error("could not convert command text to UTF-8");
    }
    return result;
}

Json describe(const sketch::DocumentSnapshot& snapshot) {
    std::map<std::string, std::size_t> types;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        ++types[entity.type];
    }
    return {{"document_id", snapshot.document_id()}, {"revision", snapshot.revision()},
            {"entity_count", snapshot.entities().size()}, {"entity_types", types},
            {"asset_count", snapshot.assets().size()}, {"history_records", snapshot.history().size()},
            {"named_revisions", snapshot.named_revisions()}, {"editable", snapshot.is_editable()},
            {"read_only_reason", snapshot.read_only_reason()}};
}

const char* archive_role_name(sketch::ArchiveRole role) {
    return role == sketch::ArchiveRole::ordinary ? "ordinary" : "recovery_copy";
}

Json recovery_summary(const sketch::RecoveryLedger& ledger,
                      const std::string& status,
                      const std::string& diagnostic) {
    std::map<std::string, std::size_t> kinds;
    for (const auto& record : ledger) ++kinds[record.record_kind];
    return {{"status", status}, {"record_count", ledger.size()},
            {"record_kinds", kinds}, {"diagnostic", diagnostic}};
}

struct LoadedProject {
    std::optional<sketch::LoadResult> document;
    std::optional<sketch::ArchiveLoadResult> archive;

    [[nodiscard]] bool is_archive() const noexcept {
        return archive && archive->supported();
    }
    [[nodiscard]] bool opaque() const noexcept {
        return archive && archive->opaque();
    }
    [[nodiscard]] sketch::DocumentSnapshot snapshot() const {
        return is_archive() ? archive->archive->document()
                           : document->document.snapshot();
    }
    [[nodiscard]] const std::string& file_sha256() const {
        return archive ? archive->file_sha256 : document->file_sha256;
    }
    [[nodiscard]] const sketch::RecoveryLedger& ledger() const {
        if (is_archive()) return archive->archive->recovery();
        static const sketch::RecoveryLedger empty;
        if (opaque() && archive->recovery.original_ledger)
            return *archive->recovery.original_ledger;
        return empty;
    }
    [[nodiscard]] sketch::ArchiveRole role() const {
        return archive->archive->role();
    }
    [[nodiscard]] std::string recovery_diagnostic() const {
        return archive ? archive->recovery.diagnostic : std::string{};
    }
};

LoadedProject load_project(const std::filesystem::path& file) {
    std::exception_ptr document_error;
    try {
        LoadedProject result;
        result.document.emplace(sketch::ProjectStore::load(file));
        return result;
    } catch (const sketch::StorageError& error) {
        if (error.code() != sketch::StorageErrorCode::unsupported_format) throw;
        document_error = std::current_exception();
    }

    std::optional<sketch::ArchiveLoadResult> opaque;
    for (const auto role : {sketch::ArchiveRole::ordinary,
                            sketch::ArchiveRole::recovery_copy}) {
        try {
            auto loaded = sketch::ProjectStore::load_archive(file, role);
            if (loaded.supported()) {
                LoadedProject result;
                result.archive.emplace(std::move(loaded));
                return result;
            }
            if (!opaque && loaded.opaque()) opaque.emplace(std::move(loaded));
        } catch (const sketch::StorageError& error) {
            if (error.code() != sketch::StorageErrorCode::unsupported_format) throw;
        }
    }
    if (opaque) {
        LoadedProject result;
        result.archive.emplace(std::move(*opaque));
        return result;
    }
    std::rethrow_exception(document_error);
}

void require_supported(const LoadedProject& loaded) {
    if (loaded.opaque()) {
        throw std::runtime_error("Opaque recovery archive cannot be validated, migrated, or extracted: " +
                                 loaded.recovery_diagnostic());
    }
}

Json describe_loaded(const LoadedProject& loaded) {
    if (loaded.opaque()) {
        return {{"file_sha256", loaded.file_sha256()},
                {"archive_role", "unknown"},
                {"editable", false},
                {"read_only_reason", loaded.recovery_diagnostic()},
                {"validation_complete", false},
                {"recovery_summary", recovery_summary(loaded.ledger(), "opaque",
                                                       loaded.recovery_diagnostic())}};
    }
    auto info = describe(loaded.snapshot());
    info["file_sha256"] = loaded.file_sha256();
    if (loaded.is_archive()) {
        info["archive_role"] = archive_role_name(loaded.role());
        info["validation_complete"] = true;
        info["recovery_summary"] = recovery_summary(loaded.ledger(), "supported", {});
    }
    return info;
}


int run(int argc, wchar_t** argv) {
    const auto usage = [] {
        std::cerr << "Usage: vertex-cli <new|inspect|validate> <project.bldproj>\n"
                  << "       vertex-cli extract <project.bldproj> <new-directory>\n"
                  << "       vertex-cli migrate <project.bldproj> <new-project.bldproj>\n"
                  << "       vertex-cli resources <project-package-directory>\n"
                  << "       vertex-cli symbols [query] [category]\n";
    };
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::wstring command(argv[1]);
    if (command == L"symbols" && argc >= 2 && argc <= 4) {
        const auto catalog = sketch::default_symbol_catalog();
        const auto query = argc >= 3 ? utf8_text(argv[2]) : std::string{};
        const auto category = argc == 4 ? utf8_text(argv[3]) : std::string{};
        const auto filtered = sketch::filter_symbol_catalog(catalog, query, category);
        auto manifest = sketch::encode_symbol_catalog_manifest(filtered);
        manifest["query"] = query;
        manifest["category"] = category;
        manifest["catalog_entry_count"] = catalog.size();
        manifest["filtered_entry_count"] = filtered.size();
        std::cout << manifest.dump(2) << '\n';
        return 0;
    }
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::filesystem::path file(argv[2]);
    if (command == L"new" && argc == 3) {
        auto document = sketch::Document::create();
        const auto receipt = sketch::ProjectStore::save(file, document.snapshot());
        auto info = describe(document.snapshot());
        info["file_sha256"] = receipt.file_sha256;
        std::cout << info.dump(2) << '\n';
        return 0;
    }
    if ((command == L"inspect" || command == L"validate") && argc == 3) {
        const auto loaded = load_project(file);
        if (command == L"validate") require_supported(loaded);
        auto info = describe_loaded(loaded);
        if (command == L"validate") {
            info["storage_integrity"] = "valid";
            info["validation_scope"] = loaded.is_archive()
                ? "format, revision history, structural references, logical digest, asset bytes and recovery ledger"
                : "format, revision history, structural references, logical digest and asset bytes";
            info["production_or_geometry_certification"] = false;
        }
        std::cout << info.dump(2) << '\n';
        return 0;
    }
    if (command == L"migrate" && argc == 4) {
        const auto destination = std::filesystem::absolute(argv[3]);
        const auto loaded = load_project(file);
        require_supported(loaded);
        const auto source_hash = loaded.file_sha256();
        const auto source_revision = loaded.snapshot().revision();
        const auto required_format = std::max(
            loaded.is_archive() ? 4U : 1U,
            sketch::ProjectStore::required_format_version(loaded.snapshot()));
        // Both save routes refuse an existing destination and leave the source
        // untouched; the archive route carries its ledger and role forward.
        const auto receipt = loaded.is_archive()
            ? sketch::ProjectStore::save_archive(destination, *loaded.archive->archive)
            : sketch::ProjectStore::save(destination, loaded.snapshot());
        Json result = {{"migrated_from", utf8(file)},
                       {"migrated_to", utf8(destination)},
                       {"source_file_sha256", source_hash},
                       {"destination_file_sha256", receipt.file_sha256},
                       {"source_revision", source_revision},
                       {"destination_revision", receipt.revision},
                       {"format_version", required_format},
                       {"source_preserved", sketch::ProjectStore::file_sha256(file) == source_hash}};
        if (loaded.is_archive()) {
            result["archive_role"] = archive_role_name(loaded.role());
            result["recovery_summary"] = recovery_summary(loaded.ledger(), "supported", {});
        }
        std::cout << result.dump(2) << '\n';
        return 0;
    }
    if (command == L"extract" && argc == 4) {
        const auto loaded = load_project(file);
        require_supported(loaded);
        const auto destination = std::filesystem::absolute(argv[3]);
        if (loaded.is_archive())
            sketch::extract_project_archive(*loaded.archive->archive, destination);
        else
            sketch::extract_project(loaded.snapshot(), destination);
        Json result = {{"extracted_to", utf8(destination)},
                       {"revision", loaded.snapshot().revision()}};
        if (loaded.is_archive()) result["archive_role"] = archive_role_name(loaded.role());
        std::cout << result.dump() << '\n';
        return 0;
    }
    if (command == L"resources" && argc == 3) {
        const auto catalog = sketch::ProjectResourceCatalog::register_package(file);
        Json resources = Json::array();
        for (const auto& resource : catalog.resources()) {
            resources.push_back({{"kind", sketch::project_resource_kind_name(resource.kind)},
                                 {"name", resource.name},
                                 {"path", resource.relative_path.generic_string()},
                                 {"sha256", resource.sha256}, {"size", resource.size}});
        }
        std::cout << Json({{"package_root", utf8(catalog.package_root())},
                           {"resource_count", catalog.resources().size()},
                           {"resources", std::move(resources)},
                           {"network_required", false}})
                         .dump(2)
                  << '\n';
        return 0;
    }
    throw std::runtime_error("Unknown command or incorrect argument count");
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << Json({{"error", error.what()}}).dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
        return 1;
    }
}
