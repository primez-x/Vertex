#pragma once

#include "sketch/recovery_copy_record.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

enum class RecoverySourceMatch {
    not_requested, source_unavailable, missing_provenance, invalid_source_path,
    different_path, hash_mismatch, matched
};

struct RecoveryCandidate {
    std::filesystem::path path;
    std::optional<RecoveryCopyRecord> metadata;
    std::string file_sha256;
    bool loadable{};
    bool duplicate_archive_id{};
    std::string reason;
    RecoverySourceMatch source_match = RecoverySourceMatch::not_requested;
};

struct RecoveryDiscoveryResult {
    std::vector<RecoveryCandidate> candidates;
    std::optional<std::string> source_sha256;
    std::string source_diagnostic;
    // Nonempty means directory enumeration was rejected or incomplete. No
    // partial candidates are returned in that case.
    std::string directory_diagnostic;
};

// Returns true only for a validated candidate whose persisted workspace has
// moved beyond its explicit-save generation. The checkpoint comparison also
// retains pointer-only authoring state, which can advance independently of
// semantic edits.
[[nodiscard]] bool recovery_candidate_has_unsaved_work(
    const RecoveryCandidate& candidate) noexcept;

// Read-only, nonrecursive discovery. A missing directory is an empty result.
// Paths containing '..', symlinks and Windows reparse points are rejected.
// Metadata paths are compared lexically and are never opened or resolved.
// Results are sorted by filename; duplicate archive IDs remain distinct and
// are not loadable. Matching is provenance evidence, never write authority.
[[nodiscard]] RecoveryDiscoveryResult discover_recovery_copies(
    const std::optional<std::filesystem::path>& source_project,
    const std::filesystem::path& recovery_directory);

}  // namespace sketch
