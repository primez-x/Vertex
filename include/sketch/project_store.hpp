#pragma once

#include "sketch/document.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sketch {

enum class SaveFaultStage {
    none,
    after_journal_creation,
    after_database_write,
    after_validation,
    before_publish,
};

struct SaveOptions {
    // Required when destination already exists. Use the hash returned by load/save.
    std::optional<std::string> expected_destination_sha256;
    SaveFaultStage fault_stage = SaveFaultStage::none;
    // Deterministic test barrier invoked while the validated staging file is locked against
    // writes, rename, and deletion. Production callers leave this empty.
    std::function<void(const std::filesystem::path&, const std::string&)>
        after_validation_barrier;
    // Test-only barrier after an existing destination backup is flushed and verified while the
    // expected destination identity remains read-locked. Production callers leave this empty.
    std::function<void(const std::filesystem::path&, const std::string&,
                       const std::optional<std::filesystem::path>&)>
        before_publication_barrier;
};

struct SaveReceipt {
    Revision revision = 0;
    std::string file_sha256;
    std::optional<std::filesystem::path> backup_path;
};

struct LoadResult {
    Document document;
    std::string file_sha256;
};

enum class StorageErrorCode {
    io_error,
    sqlite_error,
    destination_exists,
    external_change,
    unsupported_format,
    integrity_failure,
    invalid_snapshot,
    injected_failure,
    resource_limit,
};

class StorageError final : public std::runtime_error {
public:
    StorageError(StorageErrorCode code, std::string message,
                 std::vector<std::filesystem::path> residual_paths = {});
    [[nodiscard]] StorageErrorCode code() const noexcept;
    [[nodiscard]] const std::vector<std::filesystem::path>& residual_paths() const noexcept;

private:
    StorageErrorCode code_;
    std::vector<std::filesystem::path> residual_paths_;
};

class ProjectStore final {
public:
    // Highest supported storage version. Legacy-only history may still be
    // written as v1; identified boundaries, dimensions or boundary drafts
    // anywhere in retained history require v2. Qualified persisted
    // boundary_authoring envelopes require v3.
    static constexpr std::uint32_t format_version = 3;
    [[nodiscard]] static std::uint32_t required_format_version(const DocumentSnapshot& snapshot);
    static constexpr std::uint64_t maximum_file_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t maximum_revision_count = 10'000;
    static constexpr std::uint64_t maximum_entity_rows = 250'000;
    static constexpr std::uint64_t maximum_asset_rows = 100'000;
    static constexpr std::uint64_t maximum_encoded_json_bytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t maximum_json_values = 2'000'000;
    static constexpr std::uint64_t maximum_total_asset_bytes = 512ULL * 1024ULL * 1024ULL;

    [[nodiscard]] static SaveReceipt save(const std::filesystem::path& destination,
                                          const DocumentSnapshot& snapshot,
                                          const SaveOptions& options = {});
    [[nodiscard]] static LoadResult load(const std::filesystem::path& source);
    [[nodiscard]] static std::string file_sha256(const std::filesystem::path& source);
};

}  // namespace sketch
