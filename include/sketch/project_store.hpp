#pragma once

#include "sketch/document.hpp"
#include "sketch/recovery_ledger.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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

class ProjectArchiveSnapshot final {
public:
    ProjectArchiveSnapshot(DocumentSnapshot document, RecoveryLedger recovery, ArchiveRole role)
        : document_(std::move(document)), recovery_(std::move(recovery)), role_(role) {}
    [[nodiscard]] const DocumentSnapshot& document() const noexcept { return document_; }
    [[nodiscard]] const RecoveryLedger& recovery() const noexcept { return recovery_; }
    [[nodiscard]] ArchiveRole role() const noexcept { return role_; }
private:
    DocumentSnapshot document_;
    RecoveryLedger recovery_;
    ArchiveRole role_;
};
struct ArchiveLoadResult {
    // Opaque results deliberately have no forkable document snapshot. Their
    // ledger and exact source fingerprint remain available for opaque handling.
    std::optional<ProjectArchiveSnapshot> archive;
    RecoveryLedgerDecodeResult recovery;
    std::string file_sha256;
    // No editable Document is returned, especially for an opaque ledger.
    [[nodiscard]] bool supported() const noexcept { return archive.has_value() && recovery.supported(); }
    [[nodiscard]] bool opaque() const noexcept { return recovery.opaque(); }
    [[nodiscard]] bool editable() const noexcept { return supported() && archive->document().is_editable(); }
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
    // boundary_authoring envelopes require v3. Translation command proofs
    // require v5; general transform proofs require v6; boundary geometry edit
    // proofs require v7; boundary constraint transactions require v8. Any may include
    // the optional recovery ledger. Absent proofs preserve historical formats.
    static constexpr std::uint32_t format_version = 8;
    static constexpr std::uint32_t recovery_format_version = 4;
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
    // Recovery-bearing v4 through v8 only. A document-only path never drops a ledger.
    [[nodiscard]] static SaveReceipt save_archive(const std::filesystem::path& destination,
        const ProjectArchiveSnapshot&, const SaveOptions& options = {});
    [[nodiscard]] static ArchiveLoadResult load_archive(const std::filesystem::path& source, ArchiveRole role);
    [[nodiscard]] static std::string file_sha256(const std::filesystem::path& source);
};

}  // namespace sketch
