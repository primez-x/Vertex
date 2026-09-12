#pragma once

#include "sketch/project_store.hpp"
#include "sketch/workspace_ownership_broker.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace sketch {

// A point-in-time identity for a project pathname.  The path digest prevents
// cooperating sessions from opening the same lexical path, while the file
// identity and content digest detect replacement or external edits after the
// session has been acquired.
struct ProjectFileIdentity final {
    std::filesystem::path path;
    std::string path_digest;
    std::string file_digest;
    std::uint64_t volume_serial = 0;
    std::uint64_t file_index_high = 0;
    std::uint64_t file_index_low = 0;
    std::uint64_t file_size = 0;
    std::uint64_t last_write_ticks = 0;
    bool exists = false;

    friend bool operator==(const ProjectFileIdentity&, const ProjectFileIdentity&) = default;
};

enum class ProjectOwnershipStatus : std::uint8_t {
    acquired,
    released,
    conflict,
    already_owned,
    invalid_path,
    missing,
    external_change,
    backend_failure,
    not_owned,
};

struct ProjectOwnershipResult final {
    ProjectOwnershipStatus status = ProjectOwnershipStatus::backend_failure;
    std::string message;
    bool abandonment_observed = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == ProjectOwnershipStatus::acquired ||
               status == ProjectOwnershipStatus::released;
    }
};

// Owns one project path for the lifetime of a desktop session.  All mutex
// operations remain on WorkspaceOwnershipBroker's owner thread.  This class
// deliberately keeps the path and file identity evidence private to the
// session; callers can only query status and revalidate before publication.
class ProjectOwnershipSession final {
public:
    explicit ProjectOwnershipSession(
        WorkspaceOwnershipBroker& broker = WorkspaceOwnershipBroker::instance());
    ~ProjectOwnershipSession();

    ProjectOwnershipSession(const ProjectOwnershipSession&) = delete;
    ProjectOwnershipSession& operator=(const ProjectOwnershipSession&) = delete;
    ProjectOwnershipSession(ProjectOwnershipSession&& other) noexcept;
    ProjectOwnershipSession& operator=(ProjectOwnershipSession&& other) noexcept;

    // Acquires a path lease and records the current file identity.  Existing
    // paths receive an additional identity lease; missing files reserve the
    // path so a cooperating second session cannot race creation.
    [[nodiscard]] ProjectOwnershipResult acquire(const std::filesystem::path& path);

    // Rechecks path, file identity, and content against the last accepted
    // publication.  A mismatch is an explicit external-change result.
    [[nodiscard]] ProjectOwnershipResult verify_current() const;

    // Records a successful atomic publication.  The path lease remains the
    // same, while an atomic replace may legitimately produce a new file ID.
    [[nodiscard]] ProjectOwnershipResult note_published(std::string file_digest);

    [[nodiscard]] ProjectOwnershipResult release();
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool path_matches(const std::filesystem::path& path) const noexcept;
    [[nodiscard]] const ProjectFileIdentity* identity() const noexcept;

private:
    WorkspaceOwnershipBroker* broker_{};
    std::optional<WorkspaceInstanceId> workspace_;
    std::optional<WorkspaceOwnershipReservation> reservation_;
    ProjectFileIdentity identity_;
    bool released_ = true;
};

}  // namespace sketch
