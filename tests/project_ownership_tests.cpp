#include "sketch/project_ownership.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using sketch::ProjectOwnershipSession;
using sketch::ProjectOwnershipStatus;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path temporary_path(std::string_view suffix) {
    return std::filesystem::temp_directory_path() /
           ("property-studio-ownership-" + sketch::make_stable_id() + std::string(suffix));
}

void write_text(const std::filesystem::path& path, std::string_view value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "ownership fixture could not be opened for writing");
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(output.good(), "ownership fixture could not be written");
}

void test_content_and_identity_changes_are_detected() {
    const auto path = temporary_path(".bldproj");
    const auto replacement = temporary_path("-replacement.bldproj");
    write_text(path, "version-one");

    try {
        ProjectOwnershipSession first;
        require(first.acquire(path).status == ProjectOwnershipStatus::acquired,
                "first session should acquire an existing project");
        require(first.active() && first.identity() && first.identity()->exists,
                "acquired session should retain existing file identity");

        ProjectOwnershipSession second;
        require(second.acquire(path).status == ProjectOwnershipStatus::conflict,
                "second session should be blocked by the path/file lease");
#ifdef _WIN32
        auto case_variant = path;
        case_variant.replace_extension(L".BLDPROJ");
        ProjectOwnershipSession case_variant_session;
        require(case_variant_session.acquire(case_variant).status == ProjectOwnershipStatus::conflict,
                "Windows path identity should remain case-insensitive");
#endif
        require(first.verify_current().status == ProjectOwnershipStatus::acquired,
                "unchanged project should verify while leased");

        write_text(path, "version-two");
        require(first.verify_current().status == ProjectOwnershipStatus::external_change,
                "external content edit must invalidate the session evidence");

        const auto published = sketch::ProjectStore::file_sha256(path);
        require(first.note_published(published).status == ProjectOwnershipStatus::acquired,
                "a verified publication should refresh file identity evidence");
        require(first.verify_current().status == ProjectOwnershipStatus::acquired,
                "refreshed publication should verify cleanly");

        write_text(replacement, "replacement");
        std::error_code error;
        std::filesystem::rename(replacement, path, error);
        require(!error, "replacement fixture could not replace the project");
        require(first.verify_current().status == ProjectOwnershipStatus::external_change,
                "atomic path replacement must invalidate the session evidence");

        require(first.release().status == ProjectOwnershipStatus::released,
                "first session should release its lease");
        require(second.acquire(path).status == ProjectOwnershipStatus::acquired,
                "a released lease should be available to the next session");
        require(second.release().status == ProjectOwnershipStatus::released,
                "second session should release its lease");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(replacement, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(replacement, ignored);
}

void test_missing_path_is_reserved_and_creation_is_detected() {
    const auto path = temporary_path("-missing.bldproj");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    ProjectOwnershipSession session;
    require(session.acquire(path).status == ProjectOwnershipStatus::acquired,
            "missing destination should still receive a path reservation");
    require(session.identity() && !session.identity()->exists,
            "missing destination identity should record the absent state");
    write_text(path, "created outside session");
    require(session.verify_current().status == ProjectOwnershipStatus::external_change,
            "creation of a previously missing destination must be detected");
    require(session.release().status == ProjectOwnershipStatus::released,
            "missing-path session should release cleanly");
    std::filesystem::remove(path, ignored);
}

void test_path_validation_is_fail_closed() {
    ProjectOwnershipSession session;
    require(session.acquire({}).status == ProjectOwnershipStatus::invalid_path,
            "empty path must be rejected");
    const auto directory = temporary_path("-directory");
    std::error_code error;
    std::filesystem::create_directory(directory, error);
    require(!error, "directory fixture could not be created");
    require(session.acquire(directory).status == ProjectOwnershipStatus::invalid_path,
            "directory path must be rejected");
    std::filesystem::remove(directory, error);
}

}  // namespace

int main() {
    try {
        test_content_and_identity_changes_are_detected();
        test_missing_path_is_reserved_and_creation_is_detected();
        test_path_validation_is_fail_closed();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project_ownership_tests: " << error.what() << '\n';
        return 1;
    }
}
