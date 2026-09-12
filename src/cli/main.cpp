#include "sketch/document.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace {
using Json = nlohmann::json;
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
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


int run(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "Usage: property-cli <new|inspect|validate> <project.bldproj>\n"
                  << "       property-cli extract <project.bldproj> <new-directory>\n"
                  << "       property-cli migrate <project.bldproj> <new-project.bldproj>\n";
        return 2;
    }
    const std::wstring command(argv[1]);
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
        const auto loaded = sketch::ProjectStore::load(file);
        auto info = describe(loaded.document.snapshot());
        info["file_sha256"] = loaded.file_sha256;
        if (command == L"validate") {
            info["storage_integrity"] = "valid";
            info["validation_scope"] = "format, revision history, structural references, logical digest and asset bytes";
            info["production_or_geometry_certification"] = false;
        }
        std::cout << info.dump(2) << '\n';
        return 0;
    }
    if (command == L"migrate" && argc == 4) {
        const auto destination = std::filesystem::absolute(argv[3]);
        const auto loaded = sketch::ProjectStore::load(file);
        const auto source_hash = loaded.file_sha256;
        const auto source_revision = loaded.document.revision();
        const auto required_format = sketch::ProjectStore::required_format_version(
            loaded.document.snapshot());
        // ProjectStore::save is copy based: it refuses an existing destination,
        // validates the complete snapshot, and leaves the source untouched.
        const auto receipt = sketch::ProjectStore::save(destination,
                                                         loaded.document.snapshot());
        std::cout << Json({{"migrated_from", utf8(file)},
                           {"migrated_to", utf8(destination)},
                           {"source_file_sha256", source_hash},
                           {"destination_file_sha256", receipt.file_sha256},
                           {"source_revision", source_revision},
                           {"destination_revision", receipt.revision},
                           {"format_version", required_format},
                           {"source_preserved", sketch::ProjectStore::file_sha256(file) == source_hash}})
                     .dump(2)
                  << '\n';
        return 0;
    }
    if (command == L"extract" && argc == 4) {
        const auto loaded = sketch::ProjectStore::load(file);
        const auto destination = std::filesystem::absolute(argv[3]);
        sketch::extract_project(loaded.document.snapshot(), destination);
        std::cout << Json({{"extracted_to", utf8(destination)}, {"revision", loaded.document.revision()}}).dump() << '\n';
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
