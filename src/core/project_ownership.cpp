#include "sketch/project_ownership.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace sketch {
namespace {

constexpr std::uint64_t kMaximumPathBytes = 32ULL * 1024ULL;

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::string path_key_text(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto wide = path.wstring();
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("project path is too long");
    if (wide.empty()) return {};
    std::wstring folded = wide;
    const auto count = static_cast<int>(wide.size());
    if (LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, wide.data(), count,
                     folded.data(), count) != count)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "cannot normalize project path identity");
    return path_utf8(std::filesystem::path(folded));
#else
    return path_utf8(path);
#endif
}

std::filesystem::path normalize_path(const std::filesystem::path& input) {
    if (input.empty()) throw std::invalid_argument("project path is empty");
    std::error_code error;
    auto absolute = std::filesystem::absolute(input, error);
    if (error || absolute.empty()) throw std::invalid_argument("project path is not absolute");
    absolute = absolute.lexically_normal();
    const auto encoded = path_utf8(absolute);
    if (encoded.empty() || encoded.size() > kMaximumPathBytes ||
        encoded.find('\0') != std::string::npos) {
        throw std::invalid_argument("project path is invalid or too long");
    }
    return absolute;
}

std::string digest_text(std::string_view value) {
    const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    return sha256_hex(bytes);
}

ProjectOwnershipResult result(ProjectOwnershipStatus status, std::string message = {},
                              bool abandonment = false) {
    return {status, std::move(message), abandonment};
}

std::string status_message(ProjectOwnershipStatus status) {
    switch (status) {
    case ProjectOwnershipStatus::acquired: return "project ownership acquired";
    case ProjectOwnershipStatus::released: return "project ownership released";
    case ProjectOwnershipStatus::conflict: return "the project is already open by another session";
    case ProjectOwnershipStatus::already_owned: return "this session already owns a project";
    case ProjectOwnershipStatus::invalid_path: return "the project path is invalid";
    case ProjectOwnershipStatus::missing: return "the project path does not exist";
    case ProjectOwnershipStatus::external_change: return "the project changed outside this session";
    case ProjectOwnershipStatus::backend_failure: return "project ownership backend failure";
    case ProjectOwnershipStatus::not_owned: return "this session does not own a project";
    }
    return "unknown project ownership status";
}

#ifdef _WIN32
std::uint64_t filetime_ticks(const FILETIME& value) noexcept {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}
#endif

ProjectFileIdentity inspect_identity(const std::filesystem::path& path) {
    ProjectFileIdentity identity;
    identity.path = normalize_path(path);
    const auto text = path_key_text(identity.path);
    identity.path_digest = digest_text(text);

#ifdef _WIN32
    const auto handle = CreateFileW(identity.path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                        FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            identity.exists = false;
            return identity;
        }
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot inspect project identity");
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        throw std::invalid_argument("project path is a reparse point");
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        const auto error = GetLastError();
        CloseHandle(handle);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot read project identity");
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(handle);
        throw std::invalid_argument("project path is a directory");
    }
    identity.exists = true;
    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_index_high = information.nFileIndexHigh;
    identity.file_index_low = information.nFileIndexLow;
    identity.file_size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
                         information.nFileSizeLow;
    identity.last_write_ticks = filetime_ticks(information.ftLastWriteTime);
    CloseHandle(handle);
#else
    struct stat information{};
    if (::stat(identity.path.c_str(), &information) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            identity.exists = false;
            return identity;
        }
        throw std::system_error(errno, std::generic_category(), "cannot inspect project identity");
    }
    if (!S_ISREG(information.st_mode)) throw std::invalid_argument("project path is not a file");
    identity.exists = true;
    identity.volume_serial = static_cast<std::uint64_t>(information.st_dev);
    identity.file_index_low = static_cast<std::uint64_t>(information.st_ino);
    identity.file_size = static_cast<std::uint64_t>(information.st_size);
    identity.last_write_ticks = static_cast<std::uint64_t>(information.st_mtime);
#endif

    identity.file_digest = ProjectStore::file_sha256(identity.path);
    return identity;
}

bool same_file_identity(const ProjectFileIdentity& left, const ProjectFileIdentity& right) {
    if (left.exists != right.exists) return false;
    if (!left.exists) return left.path_digest == right.path_digest;
    return left.path_digest == right.path_digest &&
           left.volume_serial == right.volume_serial &&
           left.file_index_high == right.file_index_high &&
           left.file_index_low == right.file_index_low &&
           left.file_size == right.file_size &&
           left.last_write_ticks == right.last_write_ticks;
}

WorkspaceOwnershipKey path_key(const std::string& digest) {
    return WorkspaceOwnershipKey::path(CanonicalSha256::from_hex(digest));
}

WorkspaceOwnershipKey file_key(const ProjectFileIdentity& identity) {
    std::ostringstream value;
    value << identity.volume_serial << ':' << identity.file_index_high << ':'
          << identity.file_index_low;
    return WorkspaceOwnershipKey::file(CanonicalSha256::from_hex(digest_text(value.str())));
}

}  // namespace

ProjectOwnershipSession::ProjectOwnershipSession(WorkspaceOwnershipBroker& broker)
    : broker_(&broker) {}

ProjectOwnershipSession::~ProjectOwnershipSession() {
    (void)release();
}

ProjectOwnershipSession::ProjectOwnershipSession(ProjectOwnershipSession&& other) noexcept
    : broker_(other.broker_), workspace_(std::move(other.workspace_)),
      reservation_(std::move(other.reservation_)), identity_(std::move(other.identity_)),
      released_(other.released_) {
    other.workspace_.reset();
    other.reservation_.reset();
    other.identity_ = {};
    other.released_ = true;
}

ProjectOwnershipSession& ProjectOwnershipSession::operator=(ProjectOwnershipSession&& other) noexcept {
    if (this == &other) return *this;
    (void)release();
    broker_ = other.broker_;
    workspace_ = std::move(other.workspace_);
    reservation_ = std::move(other.reservation_);
    identity_ = std::move(other.identity_);
    released_ = other.released_;
    other.workspace_.reset();
    other.reservation_.reset();
    other.identity_ = {};
    other.released_ = true;
    return *this;
}

ProjectOwnershipResult ProjectOwnershipSession::acquire(const std::filesystem::path& path) {
    if (active()) return result(ProjectOwnershipStatus::already_owned, status_message(ProjectOwnershipStatus::already_owned));
    if (broker_ == nullptr) return result(ProjectOwnershipStatus::backend_failure, status_message(ProjectOwnershipStatus::backend_failure));

    ProjectFileIdentity candidate;
    try {
        candidate = inspect_identity(path);
    } catch (const std::invalid_argument& error) {
        return result(ProjectOwnershipStatus::invalid_path, error.what());
    } catch (const StorageError& error) {
        return result(ProjectOwnershipStatus::backend_failure, error.what());
    } catch (const std::exception& error) {
        return result(ProjectOwnershipStatus::backend_failure, error.what());
    }

    const auto instance = broker_->new_workspace_instance();
    if (!instance) return result(ProjectOwnershipStatus::backend_failure, status_message(ProjectOwnershipStatus::backend_failure));
    std::vector<WorkspaceOwnershipKey> keys{path_key(candidate.path_digest)};
    if (candidate.exists) keys.push_back(file_key(candidate));
    const auto acquired = broker_->acquire(*instance, WorkspaceOwnershipBundle(std::move(keys)));
    if (!acquired.ok() || !acquired.reservation) {
        if (acquired.status == WorkspaceOwnershipStatus::conflict)
            return result(ProjectOwnershipStatus::conflict, acquired.message,
                          acquired.abandonment_observed);
        return result(ProjectOwnershipStatus::backend_failure, acquired.message,
                      acquired.abandonment_observed);
    }
    workspace_ = instance;
    reservation_ = acquired.reservation;
    identity_ = std::move(candidate);
    released_ = false;
    return result(ProjectOwnershipStatus::acquired, status_message(ProjectOwnershipStatus::acquired),
                  acquired.abandonment_observed);
}

ProjectOwnershipResult ProjectOwnershipSession::verify_current() const {
    if (!active()) return result(ProjectOwnershipStatus::not_owned, status_message(ProjectOwnershipStatus::not_owned));
    try {
        const auto current = inspect_identity(identity_.path);
        if (!same_file_identity(identity_, current) ||
            (current.exists && current.file_digest != identity_.file_digest)) {
            return result(ProjectOwnershipStatus::external_change,
                          status_message(ProjectOwnershipStatus::external_change));
        }
        return result(ProjectOwnershipStatus::acquired, status_message(ProjectOwnershipStatus::acquired));
    } catch (const std::exception& error) {
        return result(ProjectOwnershipStatus::external_change, error.what());
    }
}

ProjectOwnershipResult ProjectOwnershipSession::note_published(std::string file_digest) {
    if (!active()) return result(ProjectOwnershipStatus::not_owned, status_message(ProjectOwnershipStatus::not_owned));
    if (!CanonicalSha256::parse(file_digest))
        return result(ProjectOwnershipStatus::backend_failure, "published project digest is invalid");
    try {
        auto current = inspect_identity(identity_.path);
        if (!current.exists || current.file_digest != file_digest) {
            return result(ProjectOwnershipStatus::external_change,
                          "published project identity or digest could not be verified");
        }
        current.file_digest = std::move(file_digest);
        identity_ = std::move(current);
        return result(ProjectOwnershipStatus::acquired, status_message(ProjectOwnershipStatus::acquired));
    } catch (const std::exception& error) {
        return result(ProjectOwnershipStatus::external_change, error.what());
    }
}

ProjectOwnershipResult ProjectOwnershipSession::release() {
    if (!active()) {
        released_ = true;
        return result(ProjectOwnershipStatus::released, status_message(ProjectOwnershipStatus::released));
    }
    if (broker_ == nullptr || !reservation_) {
        workspace_.reset();
        reservation_.reset();
        identity_ = {};
        released_ = true;
        return result(ProjectOwnershipStatus::released, status_message(ProjectOwnershipStatus::released));
    }
    const auto released = broker_->release(*reservation_);
    if (!released.ok()) {
        return result(ProjectOwnershipStatus::backend_failure, released.message,
                      released.abandonment_observed);
    }
    workspace_.reset();
    reservation_.reset();
    identity_ = {};
    released_ = true;
    return result(ProjectOwnershipStatus::released, status_message(ProjectOwnershipStatus::released),
                  released.abandonment_observed);
}

bool ProjectOwnershipSession::active() const noexcept {
    return !released_ && workspace_.has_value() && reservation_.has_value() &&
           workspace_->valid() && reservation_->valid();
}

bool ProjectOwnershipSession::path_matches(const std::filesystem::path& path) const noexcept {
    if (!active()) return false;
    try {
        const auto normalized = normalize_path(path);
        return digest_text(path_key_text(normalized)) == identity_.path_digest;
    } catch (...) {
        return false;
    }
}

const ProjectFileIdentity* ProjectOwnershipSession::identity() const noexcept {
    return active() ? &identity_ : nullptr;
}

}  // namespace sketch
