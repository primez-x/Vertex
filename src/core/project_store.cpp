#include "sketch/project_store.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"

#include <sqlite3.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <system_error>

namespace sketch {

namespace {
bool supported_identified_boundary_model(const Entity& entity) noexcept {
    if (!can_recognize_boundary_entity_type(entity.type) || !entity.properties.is_object()) {
        return false;
    }
    const auto model = entity.properties.find("boundary_model_version");
    if (model == entity.properties.end()) return false;
    if (model->is_number_unsigned()) return model->get<std::uint64_t>() == 1;
    return model->is_number_integer() && model->get<std::int64_t>() == 1;
}
}  // namespace

std::uint32_t ProjectStore::required_format_version(const DocumentSnapshot& snapshot) {
    std::uint32_t required = 1;
    for (const auto& revision : snapshot.history()) {
        if (revision.boundary_translation) required = std::max(required, 5U);
        if (revision.boundary_transform) required = 6;
        for (const auto& [id, entity] : revision.entities) {
            (void)id;
            const bool identified_boundary =
                can_recognize_boundary_entity_type(entity.type) &&
                entity.properties.contains("boundary_model_version");
            // Qualify the owner before looking at the reserved property. A
            // generic entity or anonymous legacy boundary may carry a vendor
            // collision that must remain in its existing storage format.
            if (identified_boundary && supported_identified_boundary_model(entity) &&
                entity.properties.contains("boundary_authoring")) {
                required = std::max(required, 3U);
            }
            if (identified_boundary || entity.type == "boundary_draft" ||
                entity.type == "dimension") {
                required = std::max(required, 2U);
            }
        }
    }
    return required;
}

class ProjectStoreAccess final {
public:
    static DocumentSnapshot make_snapshot() { return {}; }
    static void set_identity(DocumentSnapshot& snapshot, std::string document_id, Revision revision,
                             std::optional<Revision> saved_revision) {
        snapshot.document_id_ = std::move(document_id);
        snapshot.revision_ = revision;
        snapshot.saved_revision_ = saved_revision;
    }
    static std::vector<RevisionRecord>& history(DocumentSnapshot& snapshot) {
        return snapshot.history_;
    }
    static void reserve_history(DocumentSnapshot& snapshot, std::size_t count) {
        snapshot.history_.reserve(count);
    }
    static std::map<std::string, Revision, std::less<>>& names(DocumentSnapshot& snapshot) {
        return snapshot.named_revisions_;
    }
    static Document restore_document(DocumentSnapshot snapshot) {
        return Document::restore(std::move(snapshot));
    }
};

namespace {

constexpr sqlite3_int64 kMaximumAssetBytes = 256LL * 1024LL * 1024LL;
constexpr int kMaximumJsonBytes = 1024 * 1024;
constexpr int kApplicationId = 0x50535444;  // "PSTD"

[[noreturn]] void storage_error(StorageErrorCode code, const std::string& message) {
    throw StorageError(code, message);
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

#ifdef _WIN32
std::wstring final_path_from_handle(HANDLE handle, std::string_view description) {
    const auto required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (required == 0) {
        storage_error(StorageErrorCode::io_error,
                      "cannot resolve " + std::string(description) + ", Windows error " +
                          std::to_string(GetLastError()));
    }
    std::wstring result(static_cast<std::size_t>(required) + 1, L'\0');
    const auto written = GetFinalPathNameByHandleW(
        handle, result.data(), static_cast<DWORD>(result.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (written == 0 || written >= result.size()) {
        storage_error(StorageErrorCode::io_error,
                      "cannot read resolved " + std::string(description));
    }
    result.resize(written);
    return result;
}

std::wstring invariant_case_fold(std::wstring_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        storage_error(StorageErrorCode::resource_limit,
                      "project path is too long for Windows invariant case folding");
    }
    const auto required = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0, nullptr,
                                        nullptr, 0);
    if (required <= 0) {
        storage_error(StorageErrorCode::io_error,
                      "cannot case-fold project save identity, Windows error " +
                          std::to_string(GetLastError()));
    }
    std::wstring folded(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), folded.data(), required, nullptr, nullptr,
                      0) != required) {
        storage_error(StorageErrorCode::io_error,
                      "cannot case-fold project save identity");
    }
    return folded;
}

bool invariant_equals(std::wstring_view left, std::wstring_view right) {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool invalid_standalone_filename(std::wstring_view filename) {
    if (filename.empty()) {
        return true;
    }
    for (const auto character : filename) {
        if (character < 32 || character == L'"' || character == L'<' || character == L'>' ||
            character == L'|' || character == L':' || character == L'*' || character == L'?') {
            return true;
        }
    }
    const auto dot = filename.find(L'.');
    auto base = filename.substr(0, dot);
    while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) {
        base.remove_suffix(1);
    }
    static constexpr std::array<std::wstring_view, 4> reserved = {L"CON", L"PRN", L"AUX",
                                                                  L"NUL"};
    for (const auto name : reserved) {
        if (invariant_equals(base, name)) {
            return true;
        }
    }
    if (base.size() == 4 &&
        (invariant_equals(base.substr(0, 3), L"COM") ||
         invariant_equals(base.substr(0, 3), L"LPT")) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }
    return false;
}

bool ambiguous_missing_filename(std::wstring_view filename) {
    return invalid_standalone_filename(filename) || filename.back() == L'.' ||
           filename.back() == L' ';
}

class LockedReadFile final {
public:
    explicit LockedReadFile(const std::filesystem::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::io_error,
                          "cannot lock project for a consistent read, Windows error " +
                              std::to_string(GetLastError()));
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle_, FileAttributeTagInfo, &attributes,
                                           sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            storage_error(StorageErrorCode::io_error,
                          "locked project cannot be a Windows reparse point");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            storage_error(StorageErrorCode::io_error,
                          "cannot determine locked project file size");
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > ProjectStore::maximum_file_bytes) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            storage_error(StorageErrorCode::resource_limit,
                          "project file size exceeds the format v1 resource limit");
        }
    }
    ~LockedReadFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    LockedReadFile(const LockedReadFile&) = delete;
    LockedReadFile& operator=(const LockedReadFile&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class ReservedStagingFile final {
public:
    explicit ReservedStagingFile(const std::filesystem::path& path) {
        identity_ = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (identity_ == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::io_error,
                          "cannot reserve a new temporary project file, Windows error " +
                              std::to_string(GetLastError()));
        }
    }
    ~ReservedStagingFile() {
        if (identity_ != INVALID_HANDLE_VALUE) {
            CloseHandle(identity_);
        }
    }
    ReservedStagingFile(const ReservedStagingFile&) = delete;
    ReservedStagingFile& operator=(const ReservedStagingFile&) = delete;

    void flush_written_bytes() const {
        const auto flush_handle = ReOpenFile(identity_, GENERIC_READ | GENERIC_WRITE,
                                             FILE_SHARE_READ, FILE_FLAG_WRITE_THROUGH);
        if (flush_handle == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::io_error,
                          "cannot open exact staging identity for flush, Windows error " +
                              std::to_string(GetLastError()));
        }
        if (!FlushFileBuffers(flush_handle)) {
            const auto error = GetLastError();
            CloseHandle(flush_handle);
            storage_error(StorageErrorCode::io_error,
                          "cannot flush exact staging identity, Windows error " +
                              std::to_string(error));
        }
        CloseHandle(flush_handle);
    }

    [[nodiscard]] HANDLE open_validation_handle() const {
        const auto validation =
            ReOpenFile(identity_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                       FILE_FLAG_SEQUENTIAL_SCAN);
        if (validation == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::io_error,
                          "cannot seal exact staging identity for validation, Windows error " +
                              std::to_string(GetLastError()));
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(validation, &size) || size.QuadPart < 0) {
            CloseHandle(validation);
            storage_error(StorageErrorCode::io_error,
                          "cannot determine staging project file size");
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > ProjectStore::maximum_file_bytes) {
            CloseHandle(validation);
            storage_error(StorageErrorCode::resource_limit,
                          "staging project file size exceeds the format v1 resource limit");
        }
        return validation;
    }

    [[nodiscard]] HANDLE release_identity_handle() noexcept {
        const auto result = identity_;
        identity_ = INVALID_HANDLE_VALUE;
        return result;
    }

private:
    HANDLE identity_ = INVALID_HANDLE_VALUE;
};

class StagedPublicationFile final {
public:
    StagedPublicationFile(HANDLE path_guard, HANDLE validation)
        : path_guard_(path_guard), validation_(validation) {
        if (path_guard_ == INVALID_HANDLE_VALUE || validation_ == INVALID_HANDLE_VALUE) {
            if (path_guard_ != INVALID_HANDLE_VALUE) {
                CloseHandle(path_guard_);
                path_guard_ = INVALID_HANDLE_VALUE;
            }
            if (validation_ != INVALID_HANDLE_VALUE) {
                CloseHandle(validation_);
                validation_ = INVALID_HANDLE_VALUE;
            }
            storage_error(StorageErrorCode::io_error,
                          "staging identity handle chain is invalid");
        }
    }
    ~StagedPublicationFile() {
        if (publication_ != INVALID_HANDLE_VALUE) {
            CloseHandle(publication_);
        }
        if (path_guard_ != INVALID_HANDLE_VALUE) {
            CloseHandle(path_guard_);
        }
        if (validation_ != INVALID_HANDLE_VALUE) {
            CloseHandle(validation_);
        }
    }
    StagedPublicationFile(const StagedPublicationFile&) = delete;
    StagedPublicationFile& operator=(const StagedPublicationFile&) = delete;

    [[nodiscard]] HANDLE validation_handle() const noexcept { return validation_; }
    [[nodiscard]] HANDLE acquire_publication_handle() {
        if (publication_ != INVALID_HANDLE_VALUE) {
            return publication_;
        }
        // The original CREATE_NEW handle denies deletion and keeps the pathname bound to the
        // validated object through SQLite close. ReOpenFile then upgrades the same object to a
        // DELETE-capable publication handle; the pathname is never reopened as the source.
        CloseHandle(path_guard_);
        path_guard_ = INVALID_HANDLE_VALUE;
        publication_ = ReOpenFile(validation_, GENERIC_READ | DELETE, FILE_SHARE_READ,
                                  FILE_FLAG_SEQUENTIAL_SCAN);
        if (publication_ == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::io_error,
                          "cannot lock validated staging identity for publication, Windows error " +
                              std::to_string(GetLastError()));
        }
        return publication_;
    }

private:
    HANDLE path_guard_ = INVALID_HANDLE_VALUE;
    HANDLE validation_ = INVALID_HANDLE_VALUE;
    HANDLE publication_ = INVALID_HANDLE_VALUE;
};

class DestinationPublicationGuard final {
public:
    explicit DestinationPublicationGuard(const std::filesystem::path& path) {
        identity_ = open_read_guard(path);
        try {
            validate_ordinary_file(identity_);
        } catch (...) {
            CloseHandle(identity_);
            identity_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }
    ~DestinationPublicationGuard() {
        if (current_name_ != INVALID_HANDLE_VALUE) {
            CloseHandle(current_name_);
        }
        if (identity_ != INVALID_HANDLE_VALUE) {
            CloseHandle(identity_);
        }
    }
    DestinationPublicationGuard(const DestinationPublicationGuard&) = delete;
    DestinationPublicationGuard& operator=(const DestinationPublicationGuard&) = delete;

    [[nodiscard]] HANDLE identity_handle() const noexcept { return identity_; }

    [[nodiscard]] HANDLE lock_and_match_current_name(const std::filesystem::path& path) {
        if (current_name_ != INVALID_HANDLE_VALUE) {
            return current_name_;
        }
        current_name_ = open_read_guard(path);
        validate_ordinary_file(current_name_);
        FILE_ID_INFO original{};
        FILE_ID_INFO current{};
        if (!GetFileInformationByHandleEx(identity_, FileIdInfo, &original, sizeof(original)) ||
            !GetFileInformationByHandleEx(current_name_, FileIdInfo, &current,
                                           sizeof(current))) {
            storage_error(StorageErrorCode::external_change,
                          "cannot revalidate project destination identity before publication");
        }
        if (original.VolumeSerialNumber != current.VolumeSerialNumber ||
            std::memcmp(&original.FileId, &current.FileId, sizeof(original.FileId)) != 0) {
            storage_error(StorageErrorCode::external_change,
                          "project destination pathname changed identity before publication");
        }
        return current_name_;
    }

private:
    static HANDLE open_read_guard(const std::filesystem::path& path) {
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                            FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::external_change,
                          "cannot lock expected project destination before publication, Windows "
                          "error " +
                              std::to_string(GetLastError()));
        }
        return handle;
    }

    static void validate_ordinary_file(HANDLE handle) {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                           sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            storage_error(StorageErrorCode::external_change,
                          "project destination became a reparse point before publication");
        }
    }

    HANDLE identity_ = INVALID_HANDLE_VALUE;
    HANDLE current_name_ = INVALID_HANDLE_VALUE;
};

class DestinationSaveMutex final {
public:
    explicit DestinationSaveMutex(const std::filesystem::path& destination) {
        const auto parent = destination.has_parent_path() ? destination.parent_path()
                                                          : std::filesystem::current_path();
        const auto parent_handle = CreateFileW(
            parent.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (parent_handle == INVALID_HANDLE_VALUE) {
            storage_error(StorageErrorCode::io_error,
                          "cannot open project parent for save identity, Windows error " +
                              std::to_string(GetLastError()));
        }
        std::wstring canonical_parent;
        try {
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!GetFileInformationByHandleEx(parent_handle, FileAttributeTagInfo, &attributes,
                                               sizeof(attributes)) ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                storage_error(StorageErrorCode::io_error,
                              "project parent identity is unavailable or is a reparse point");
            }
            canonical_parent =
                final_path_from_handle(parent_handle, "project parent identity");
        } catch (...) {
            CloseHandle(parent_handle);
            throw;
        }
        CloseHandle(parent_handle);

        std::wstring canonical;
        const auto destination_handle = CreateFileW(
            destination.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (destination_handle != INVALID_HANDLE_VALUE) {
            try {
                FILE_ATTRIBUTE_TAG_INFO destination_attributes{};
                if (!GetFileInformationByHandleEx(destination_handle, FileAttributeTagInfo,
                                                   &destination_attributes,
                                                   sizeof(destination_attributes)) ||
                    (destination_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    storage_error(
                        StorageErrorCode::io_error,
                        "project destination identity is unavailable or is a reparse point");
                }
                canonical = final_path_from_handle(destination_handle,
                                                   "project destination identity");
            } catch (...) {
                CloseHandle(destination_handle);
                throw;
            }
            CloseHandle(destination_handle);
        } else {
            const auto open_error = GetLastError();
            if (open_error != ERROR_FILE_NOT_FOUND && open_error != ERROR_PATH_NOT_FOUND) {
                storage_error(StorageErrorCode::io_error,
                              "cannot resolve project destination identity, Windows error " +
                                  std::to_string(open_error));
            }
            const auto filename = destination.filename().wstring();
            if (ambiguous_missing_filename(filename)) {
                storage_error(StorageErrorCode::io_error,
                              "missing project destination has an ambiguous Windows filename");
            }
            canonical = std::move(canonical_parent);
            canonical.push_back(L'\\');
            canonical.append(filename);
        }
        canonical = invariant_case_fold(canonical);
        const auto canonical_bytes = std::as_bytes(
            std::span<const wchar_t>(canonical.data(), canonical.size()));
        const auto digest = sha256_hex(canonical_bytes);
        const std::wstring name = L"Global\\PropertyStudio.Save." +
                                  std::wstring(digest.begin(), digest.end());
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex_ == nullptr) {
            storage_error(StorageErrorCode::io_error,
                          "cannot create project save mutex, Windows error " +
                              std::to_string(GetLastError()));
        }
        const auto wait = WaitForSingleObject(mutex_, 30'000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            storage_error(StorageErrorCode::io_error,
                          wait == WAIT_TIMEOUT ? "timed out waiting for another project save"
                                               : "cannot acquire project save mutex");
        }
        acquired_ = true;
    }
    ~DestinationSaveMutex() {
        if (acquired_) {
            ReleaseMutex(mutex_);
        }
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
    }
    DestinationSaveMutex(const DestinationSaveMutex&) = delete;
    DestinationSaveMutex& operator=(const DestinationSaveMutex&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool acquired_ = false;
};
#endif

class Database final {
public:
    explicit Database(sqlite3* handle) : handle_(handle) {}
    ~Database() {
        if (handle_ != nullptr) {
            sqlite3_close_v2(handle_);
        }
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    [[nodiscard]] sqlite3* get() const noexcept { return handle_; }
    void close() {
        if (handle_ != nullptr) {
            const auto result = sqlite3_close(handle_);
            if (result != SQLITE_OK) {
                storage_error(StorageErrorCode::sqlite_error,
                              "SQLite could not close the project database");
            }
            handle_ = nullptr;
        }
    }

private:
    sqlite3* handle_ = nullptr;
};

[[noreturn]] void sqlite_error(sqlite3* database, std::string_view operation) {
    if (database != nullptr && sqlite3_errcode(database) == SQLITE_NOMEM) {
        storage_error(StorageErrorCode::resource_limit,
                      std::string(operation) + ": SQLite allocation failed");
    }
    storage_error(StorageErrorCode::sqlite_error,
                  std::string(operation) + ": " +
                      (database == nullptr ? "SQLite error" : sqlite3_errmsg(database)));
}

Database open_database(const std::filesystem::path& path, int flags) {
    sqlite3* raw = nullptr;
    const auto encoded = path_utf8(path);
    const auto result = sqlite3_open_v2(encoded.c_str(), &raw, flags | SQLITE_OPEN_EXRESCODE, nullptr);
    if (result != SQLITE_OK) {
        const std::string message = raw == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(raw);
        if (raw != nullptr) {
            sqlite3_close_v2(raw);
        }
        storage_error(StorageErrorCode::sqlite_error, "cannot open project database: " + message);
    }
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaximumAssetBytes + kMaximumJsonBytes));
    sqlite3_limit(raw, SQLITE_LIMIT_SQL_LENGTH, kMaximumJsonBytes);
    sqlite3_limit(raw, SQLITE_LIMIT_COLUMN, 64);
    sqlite3_limit(raw, SQLITE_LIMIT_EXPR_DEPTH, 100);
    return Database(raw);
}

void execute(sqlite3* database, const char* sql) {
    char* error = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
        sqlite3_free(error);
        storage_error(StorageErrorCode::sqlite_error, message);
    }
}

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            sqlite_error(database, "cannot prepare project query");
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    void reset() {
        if (sqlite3_reset(statement_) != SQLITE_OK || sqlite3_clear_bindings(statement_) != SQLITE_OK) {
            sqlite_error(database_, "cannot reset project query");
        }
    }
    bool row() {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        sqlite_error(database_, "project query failed");
    }
    void done() {
        if (sqlite3_step(statement_) != SQLITE_DONE) {
            sqlite_error(database_, "project write failed");
        }
    }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void bind_text(sqlite3* database, sqlite3_stmt* statement, int index, std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite_error(database, "cannot bind project text");
    }
}

void bind_revision(sqlite3* database, sqlite3_stmt* statement, int index, Revision revision) {
    if (revision > static_cast<Revision>(std::numeric_limits<sqlite3_int64>::max()) ||
        sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(revision)) != SQLITE_OK) {
        sqlite_error(database, "cannot bind project revision");
    }
}

void bind_optional_revision(sqlite3* database, sqlite3_stmt* statement, int index,
                            std::optional<Revision> revision) {
    if (revision.has_value()) {
        bind_revision(database, statement, index, *revision);
    } else if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        sqlite_error(database, "cannot bind null revision");
    }
}

std::string column_text(sqlite3_stmt* statement, int column, std::size_t maximum,
                        std::string_view field) {
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " has the wrong SQLite type");
    }
    const auto size = sqlite3_column_bytes(statement, column);
    if (size < 0 || static_cast<std::size_t>(size) > maximum) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " exceeds its size limit");
    }
    const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return {value, static_cast<std::size_t>(size)};
}

Revision column_revision(sqlite3_stmt* statement, int column, std::string_view field) {
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " has the wrong SQLite type");
    }
    const auto value = sqlite3_column_int64(statement, column);
    if (value < 0) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " is negative");
    }
    return static_cast<Revision>(value);
}

std::optional<Revision> column_optional_revision(sqlite3_stmt* statement, int column,
                                                 std::string_view field) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return column_revision(statement, column, field);
}

nlohmann::json parse_json(std::string encoded, bool require_object, std::string_view field) {
    auto value = nlohmann::json::parse(encoded, nullptr, false);
    if (value.is_discarded() || (require_object && !value.is_object())) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " contains invalid JSON");
    }
    return value;
}

class DecodeBudget final {
public:
    explicit DecodeBudget(bool strict = false) : strict_(strict) {}
    bool strict() const noexcept { return strict_; }
    void add_value() {
        if (json_values_ == ProjectStore::maximum_json_values)
            storage_error(StorageErrorCode::resource_limit, "project aggregate JSON value limit exceeded");
        ++json_values_;
    }
    void add_string(std::size_t size) {
        constexpr auto maximum = WorkspaceRecoveryLimits{}.max_string_bytes;
        if (size > maximum - string_bytes_)
            storage_error(StorageErrorCode::resource_limit, "project aggregate decoded string budget exceeded");
        string_bytes_ += size;
    }
    void add_json_values(const nlohmann::json& value) {
        std::vector<const nlohmann::json*> pending{&value};
        std::uint64_t added = 0;
        while (!pending.empty()) {
            const auto* current = pending.back();
            pending.pop_back();
            ++added;
            if (added > ProjectStore::maximum_json_values - json_values_) {
                storage_error(StorageErrorCode::resource_limit,
                              "project aggregate JSON value count exceeds the format v1 limit");
            }
            if (current->is_array() || current->is_object()) {
                for (const auto& child : *current) {
                    pending.push_back(&child);
                }
            }
        }
        json_values_ += added;
    }

private:
    bool strict_ = false;
    std::uint64_t json_values_ = 0;
    std::size_t string_bytes_ = 0;
};

// Preflight the wire before constructing a DOM. Keeping keys only for open
// objects catches duplicate keys that a normal JSON DOM silently discards.
class RecoveryJsonPreflight final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit RecoveryJsonPreflight(DecodeBudget& budget) : budget_(budget) {}
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t, const string_t&) override { return scalar(); }
    bool string(string_t& value) override { budget_.add_string(value.size()); return scalar(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return start(); }
    bool start_array(std::size_t) override { return start(); }
    bool key(string_t& value) override {
        budget_.add_string(value.size());
        if (!keys_.back().insert(value).second)
            storage_error(StorageErrorCode::integrity_failure, "duplicate recovery JSON object key");
        return true;
    }
    bool end_object() override { keys_.pop_back(); return true; }
    bool end_array() override { keys_.pop_back(); return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
private:
    bool scalar() { check_depth(); budget_.add_value(); return true; }
    void check_depth() {
        if (keys_.size() > 64)
            storage_error(StorageErrorCode::resource_limit, "recovery JSON nesting exceeds 64");
    }
    bool start() {
        check_depth();
        budget_.add_value();
        keys_.emplace_back();
        return true;
    }
    DecodeBudget& budget_;
    std::vector<std::set<std::string, std::less<>>> keys_;
};

nlohmann::json parse_budgeted_json(std::string encoded, bool require_object,
                                   std::string_view field, DecodeBudget& budget) {
    if (budget.strict()) {
        RecoveryJsonPreflight preflight(budget);
        if (!nlohmann::json::sax_parse(encoded, &preflight))
            storage_error(StorageErrorCode::integrity_failure, std::string(field) + " contains invalid JSON");
    }
    auto value = parse_json(std::move(encoded), require_object, field);
    if (!budget.strict()) budget.add_json_values(value);
    return value;
}

std::vector<Revision> parse_revision_stack(std::string encoded, std::string_view field,
                                           DecodeBudget& budget) {
    const auto value = parse_budgeted_json(std::move(encoded), false, field, budget);
    if (!value.is_array() ||
        value.size() > static_cast<std::size_t>(ProjectStore::maximum_revision_count)) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " is not a bounded revision array");
    }
    std::vector<Revision> revisions;
    revisions.reserve(value.size());
    for (const auto& entry : value) {
        if (!entry.is_number_unsigned()) {
            storage_error(StorageErrorCode::integrity_failure,
                          std::string(field) + " contains an invalid revision");
        }
        revisions.push_back(entry.get<Revision>());
    }
    return revisions;
}

std::uint64_t scalar_nonnegative(sqlite3* database, const char* sql, std::string_view field) {
    Statement statement(database, sql);
    if (!statement.row() || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " aggregate is not an integer");
    }
    const auto value = sqlite3_column_int64(statement.get(), 0);
    if (value < 0 || statement.row()) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " aggregate is invalid");
    }
    return static_cast<std::uint64_t>(value);
}

struct LoadCounts {
    std::uint64_t revisions = 0;
};

LoadCounts enforce_preallocation_budgets(sqlite3* database, bool recovery = false) {
    const auto revisions = scalar_nonnegative(database, "SELECT count(*) FROM revisions",
                                              "revision count");
    const auto entities = scalar_nonnegative(database, "SELECT count(*) FROM revision_entities",
                                             "entity row count");
    const auto assets = scalar_nonnegative(database, "SELECT count(*) FROM revision_assets",
                                           "asset row count");
    const auto names = scalar_nonnegative(database, "SELECT count(*) FROM named_revisions",
                                          "named revision count");
    if (revisions > ProjectStore::maximum_revision_count) {
        storage_error(StorageErrorCode::resource_limit,
                      "project revision count exceeds the format v1 resource limit");
    }
    if (entities > ProjectStore::maximum_entity_rows) {
        storage_error(StorageErrorCode::resource_limit,
                      "project entity row count exceeds the format v1 resource limit");
    }
    if (assets > ProjectStore::maximum_asset_rows) {
        storage_error(StorageErrorCode::resource_limit,
                      "project asset row count exceeds the format v1 resource limit");
    }
    if (names > ProjectStore::maximum_revision_count) {
        storage_error(StorageErrorCode::resource_limit,
                      "project named revision count exceeds the format v1 resource limit");
    }
    const auto json_bytes = scalar_nonnegative(
        database,
        "SELECT "
        "COALESCE((SELECT sum(length(CAST(undo_stack_json AS BLOB))+"
        "length(CAST(redo_stack_json AS BLOB))) FROM revisions),0)+"
        "COALESCE((SELECT sum(length(CAST(properties_json AS BLOB))+"
        "length(CAST(extensions_json AS BLOB))) FROM "
        "revision_entities),0)+"
        "COALESCE((SELECT sum(length(CAST(metadata_json AS BLOB))) FROM revision_assets),0)",
        "encoded JSON bytes");
    const auto format = scalar_nonnegative(database, "PRAGMA user_version", "format version");
    const bool translations = format >= 5;
    const auto translation_bytes = translations ? scalar_nonnegative(database,
        "SELECT COALESCE(sum(length(CAST(boundary_translation_json AS BLOB))),0) FROM revisions",
        "boundary translation JSON bytes") : 0;
    const auto transform_bytes = format >= 6 ? scalar_nonnegative(database,
        "SELECT COALESCE(sum(length(CAST(boundary_transform_json AS BLOB))),0) FROM revisions",
        "boundary transform JSON bytes") : 0;
    const auto recovery_bytes = recovery ? scalar_nonnegative(database,
        "SELECT COALESCE(sum(length(CAST(envelope_json AS BLOB))+64+6*length(CAST(record_id AS BLOB))+"
        "6*length(CAST(record_kind AS BLOB))),0) FROM project_recovery_records", "recovery JSON bytes") : 0;
    if (recovery) {
        const auto rows = scalar_nonnegative(database, "SELECT count(*) FROM project_recovery_records",
                                             "recovery row count");
        if (rows == 0)
            storage_error(StorageErrorCode::integrity_failure, "archive has no recovery records");
        if (rows > (ProjectStore::maximum_json_values - 1) / 7)
            storage_error(StorageErrorCode::resource_limit, "recovery row count exceeds aggregate JSON budget");
    }
    if (json_bytes > ProjectStore::maximum_encoded_json_bytes ||
        translation_bytes > ProjectStore::maximum_encoded_json_bytes - json_bytes ||
        transform_bytes > ProjectStore::maximum_encoded_json_bytes - json_bytes - translation_bytes ||
        recovery_bytes > ProjectStore::maximum_encoded_json_bytes - json_bytes - translation_bytes - transform_bytes) {
        storage_error(StorageErrorCode::resource_limit,
                      "project encoded JSON bytes exceed the format v1 resource limit");
    }
    const auto asset_bytes = scalar_nonnegative(
        database, "SELECT COALESCE(sum(length(data)),0) FROM revision_assets",
        "aggregate asset bytes");
    if (asset_bytes > ProjectStore::maximum_total_asset_bytes) {
        storage_error(StorageErrorCode::resource_limit,
                      "project aggregate asset bytes exceed the format v1 resource limit");
    }
    return {revisions};
}

std::uint64_t json_value_count(const nlohmann::json& value) {
    DecodeBudget budget;
    budget.add_json_values(value);
    std::vector<const nlohmann::json*> pending{&value};
    std::uint64_t count = 0;
    while (!pending.empty()) {
        const auto* current = pending.back();
        pending.pop_back();
        ++count;
        if (current->is_array() || current->is_object()) {
            for (const auto& child : *current) {
                pending.push_back(&child);
            }
        }
    }
    return count;
}

void enforce_snapshot_budget(const DocumentSnapshot& snapshot) {
    if (snapshot.history().size() > ProjectStore::maximum_revision_count ||
        snapshot.named_revisions().size() > ProjectStore::maximum_revision_count) {
        storage_error(StorageErrorCode::resource_limit,
                      "document history exceeds the format v1 revision limit");
    }
    std::uint64_t entity_rows = 0;
    std::uint64_t asset_rows = 0;
    std::uint64_t json_bytes = 0;
    std::uint64_t json_values = 0;
    std::uint64_t asset_bytes = 0;
    auto add_json = [&](const nlohmann::json& value) {
        const auto encoded_size = value.dump().size();
        if (encoded_size > ProjectStore::maximum_encoded_json_bytes - json_bytes) {
            storage_error(StorageErrorCode::resource_limit,
                          "document encoded JSON exceeds the format v1 aggregate limit");
        }
        json_bytes += encoded_size;
        const auto values = json_value_count(value);
        if (values > ProjectStore::maximum_json_values - json_values) {
            storage_error(StorageErrorCode::resource_limit,
                          "document JSON value count exceeds the format v1 aggregate limit");
        }
        json_values += values;
    };
    for (const auto& revision : snapshot.history()) {
        add_json(nlohmann::json(revision.undo_stack));
        add_json(nlohmann::json(revision.redo_stack));
        if (revision.boundary_translation)
            add_json(encode_boundary_translation(*revision.boundary_translation));
        if (revision.boundary_transform)
            add_json(encode_boundary_transform(*revision.boundary_transform));
        for (const auto& [id, entity] : revision.entities) {
            (void)id;
            if (++entity_rows > ProjectStore::maximum_entity_rows) {
                storage_error(StorageErrorCode::resource_limit,
                              "document entity rows exceed the format v1 aggregate limit");
            }
            add_json(entity.properties);
            add_json(entity.extensions);
        }
        for (const auto& [id, asset] : revision.assets) {
            (void)id;
            if (++asset_rows > ProjectStore::maximum_asset_rows) {
                storage_error(StorageErrorCode::resource_limit,
                              "document asset rows exceed the format v1 aggregate limit");
            }
            if (asset.bytes.size() > ProjectStore::maximum_total_asset_bytes - asset_bytes) {
                storage_error(StorageErrorCode::resource_limit,
                              "document asset bytes exceed the format v1 aggregate limit");
            }
            asset_bytes += asset.bytes.size();
            add_json(asset.metadata);
        }
    }
}

std::string revisions_json(const std::vector<Revision>& revisions) {
    return nlohmann::json(revisions).dump();
}

nlohmann::json logical_manifest(const DocumentSnapshot& snapshot, std::uint32_t format,
                                const RecoveryLedger* recovery = nullptr) {
    nlohmann::json manifest = {
        {"format_version", format},
        {"document_id", snapshot.document_id()},
        {"head_revision", snapshot.revision()},
        {"saved_revision", snapshot.revision()},
        {"history", nlohmann::json::array()},
        {"named_revisions", snapshot.named_revisions()},
    };
    if (recovery) {
        manifest["saved_revision"] = snapshot.saved_revision_optional()
            ? nlohmann::json(*snapshot.saved_revision_optional()) : nlohmann::json(nullptr);
        manifest["recovery_records"] = nlohmann::json::array();
        std::vector<const RecoveryRecord*> ordered;
        ordered.reserve(recovery->size());
        for (const auto& row : *recovery) ordered.push_back(&row);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->record_id < b->record_id;
        });
        for (const auto* row : ordered)
            manifest["recovery_records"].push_back({{"record_id", row->record_id},
                {"record_kind", row->record_kind}, {"envelope", row->envelope}});
    }
    for (const auto& revision : snapshot.history()) {
        nlohmann::json encoded_revision = {
            {"revision", revision.revision},
            {"parent_revision", revision.parent_revision},
            {"source_revision", revision.source_revision},
            {"action", revision.action},
            {"name", revision.name},
            {"undo_stack", revision.undo_stack},
            {"redo_stack", revision.redo_stack},
            {"entities", nlohmann::json::array()},
            {"assets", nlohmann::json::array()},
        };
        if (revision.boundary_translation)
            encoded_revision["boundary_translation"] = encode_boundary_translation(*revision.boundary_translation);
        if (revision.boundary_transform)
            encoded_revision["boundary_transform"] = encode_boundary_transform(*revision.boundary_transform);
        for (const auto& [id, entity] : revision.entities) {
            encoded_revision["entities"].push_back({
                {"id", id},
                {"type", entity.type},
                {"required", entity.required},
                {"properties", entity.properties},
                {"extensions", entity.extensions},
            });
        }
        for (const auto& [id, asset] : revision.assets) {
            encoded_revision["assets"].push_back({
                {"id", id},
                {"media_type", asset.media_type},
                {"sha256", asset.sha256},
                {"size", asset.bytes.size()},
                {"metadata", asset.metadata},
            });
        }
        manifest["history"].push_back(std::move(encoded_revision));
    }
    return manifest;
}

std::string logical_digest(const DocumentSnapshot& snapshot, std::uint32_t format = 0,
                           const RecoveryLedger* recovery = nullptr) {
    const auto encoded = logical_manifest(snapshot, format == 0 ? ProjectStore::required_format_version(snapshot) : format, recovery).dump();
    return sha256_hex(std::as_bytes(std::span<const char>(encoded.data(), encoded.size())));
}

void put_metadata(sqlite3* database, Statement& statement, std::string_view key,
                  std::string_view value) {
    bind_text(database, statement.get(), 1, key);
    bind_text(database, statement.get(), 2, value);
    statement.done();
    statement.reset();
}

std::string write_database(const std::filesystem::path& path, const DocumentSnapshot& snapshot,
                           SaveFaultStage fault_stage, const RecoveryLedger* recovery = nullptr) {
    auto database = open_database(path, SQLITE_OPEN_READWRITE);
    execute(database.get(), "PRAGMA trusted_schema=OFF");
    execute(database.get(), "PRAGMA foreign_keys=ON");
    execute(database.get(), "PRAGMA journal_mode=DELETE");
    execute(database.get(), "PRAGMA synchronous=FULL");
    execute(database.get(), "PRAGMA locking_mode=EXCLUSIVE");
    execute(database.get(), "PRAGMA application_id=1347638340");
    const auto format = std::max(recovery ? 4U : 1U, ProjectStore::required_format_version(snapshot));
    const auto requested_digest = logical_digest(snapshot, format, recovery);
    const auto user_version = "PRAGMA user_version=" + std::to_string(format);
    execute(database.get(), user_version.c_str());
    execute(database.get(), "BEGIN IMMEDIATE");
    bool committed = false;
    try {
        execute(database.get(),
                "CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL) STRICT;");
        if (fault_stage == SaveFaultStage::after_journal_creation) {
            storage_error(StorageErrorCode::injected_failure,
                          "project save stopped after SQLite journal creation");
        }
        const std::string revision_schema =
                "CREATE TABLE revisions("
                "revision INTEGER PRIMARY KEY, parent_revision INTEGER, source_revision INTEGER, "
                "action TEXT NOT NULL, name TEXT, undo_stack_json TEXT NOT NULL, "
                "redo_stack_json TEXT NOT NULL, " +
                std::string(format >= 5 ? "boundary_translation_json TEXT, " : "") +
                std::string(format >= 6 ? "boundary_transform_json TEXT, " : "") +
                "FOREIGN KEY(parent_revision) REFERENCES revisions(revision), "
                "FOREIGN KEY(source_revision) REFERENCES revisions(revision)) STRICT;";
        execute(database.get(), revision_schema.c_str());
        execute(database.get(),
                "CREATE TABLE revision_entities("
                "revision INTEGER NOT NULL, id TEXT NOT NULL, type TEXT NOT NULL, "
                "required INTEGER NOT NULL CHECK(required IN (0,1)), properties_json TEXT NOT NULL, "
                "extensions_json TEXT NOT NULL, PRIMARY KEY(revision,id), "
                "FOREIGN KEY(revision) REFERENCES revisions(revision) ON DELETE CASCADE) STRICT;");
        execute(database.get(),
                "CREATE TABLE revision_assets("
                "revision INTEGER NOT NULL, asset_id TEXT NOT NULL, media_type TEXT NOT NULL, "
                "sha256 TEXT NOT NULL, metadata_json TEXT NOT NULL, data BLOB NOT NULL, "
                "PRIMARY KEY(revision,asset_id), "
                "FOREIGN KEY(revision) REFERENCES revisions(revision) ON DELETE CASCADE) STRICT;");
        execute(database.get(),
                "CREATE TABLE named_revisions("
                "name TEXT PRIMARY KEY, revision INTEGER NOT NULL, "
                "FOREIGN KEY(revision) REFERENCES revisions(revision)) STRICT;");

        if (recovery) {
            execute(database.get(), "CREATE TABLE project_recovery_records("
                "record_id TEXT PRIMARY KEY,record_kind TEXT NOT NULL,envelope_json TEXT NOT NULL) STRICT;");
            Statement row(database.get(), "INSERT INTO project_recovery_records VALUES(?1,?2,?3)");
            for (const auto& record : *recovery) {
                bind_text(database.get(), row.get(), 1, record.record_id);
                bind_text(database.get(), row.get(), 2, record.record_kind);
                bind_text(database.get(), row.get(), 3, record.envelope.dump());
                row.done();
                row.reset();
            }
        }
        Statement metadata(database.get(), "INSERT INTO metadata(key,value) VALUES(?1,?2)");
        put_metadata(database.get(), metadata, "format_version",
                     std::to_string(format));
        put_metadata(database.get(), metadata, "document_id", snapshot.document_id());
        put_metadata(database.get(), metadata, "head_revision", std::to_string(snapshot.revision()));
        put_metadata(database.get(), metadata, "saved_revision", recovery
            ? (snapshot.saved_revision_optional() ? std::to_string(*snapshot.saved_revision_optional()) : "null")
            : std::to_string(snapshot.revision()));
        put_metadata(database.get(), metadata, "logical_digest", requested_digest);

        Statement revision_statement(
            database.get(),
            format >= 6
                ? "INSERT INTO revisions(revision,parent_revision,source_revision,action,name,"
                  "undo_stack_json,redo_stack_json,boundary_translation_json,boundary_transform_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
                : format == 5
                ? "INSERT INTO revisions(revision,parent_revision,source_revision,action,name,"
                  "undo_stack_json,redo_stack_json,boundary_translation_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"
                : "INSERT INTO revisions(revision,parent_revision,source_revision,action,name,"
                  "undo_stack_json,redo_stack_json) VALUES(?1,?2,?3,?4,?5,?6,?7)");
        Statement entity_statement(
            database.get(),
            "INSERT INTO revision_entities(revision,id,type,required,properties_json,extensions_json) "
            "VALUES(?1,?2,?3,?4,?5,?6)");
        Statement asset_statement(
            database.get(),
            "INSERT INTO revision_assets(revision,asset_id,media_type,sha256,metadata_json,data) "
            "VALUES(?1,?2,?3,?4,?5,?6)");
        for (const auto& revision : snapshot.history()) {
            bind_revision(database.get(), revision_statement.get(), 1, revision.revision);
            bind_optional_revision(database.get(), revision_statement.get(), 2,
                                   revision.parent_revision);
            bind_optional_revision(database.get(), revision_statement.get(), 3,
                                   revision.source_revision);
            bind_text(database.get(), revision_statement.get(), 4, revision.action);
            if (revision.name.has_value()) {
                bind_text(database.get(), revision_statement.get(), 5, *revision.name);
            } else if (sqlite3_bind_null(revision_statement.get(), 5) != SQLITE_OK) {
                sqlite_error(database.get(), "cannot bind null revision name");
            }
            bind_text(database.get(), revision_statement.get(), 6,
                      revisions_json(revision.undo_stack));
            bind_text(database.get(), revision_statement.get(), 7,
                      revisions_json(revision.redo_stack));
            if (format >= 5) {
                if (revision.boundary_translation)
                    bind_text(database.get(), revision_statement.get(), 8,
                              encode_boundary_translation(*revision.boundary_translation).dump());
                else if (sqlite3_bind_null(revision_statement.get(), 8) != SQLITE_OK)
                    sqlite_error(database.get(), "cannot bind absent boundary translation");
            }
            if (format >= 6) {
                if (revision.boundary_transform)
                    bind_text(database.get(), revision_statement.get(), 9,
                              encode_boundary_transform(*revision.boundary_transform).dump());
                else if (sqlite3_bind_null(revision_statement.get(), 9) != SQLITE_OK)
                    sqlite_error(database.get(), "cannot bind absent boundary transform");
            }
            revision_statement.done();
            revision_statement.reset();

            for (const auto& [id, entity] : revision.entities) {
                bind_revision(database.get(), entity_statement.get(), 1, revision.revision);
                bind_text(database.get(), entity_statement.get(), 2, id);
                bind_text(database.get(), entity_statement.get(), 3, entity.type);
                if (sqlite3_bind_int(entity_statement.get(), 4, entity.required ? 1 : 0) != SQLITE_OK) {
                    sqlite_error(database.get(), "cannot bind entity required flag");
                }
                bind_text(database.get(), entity_statement.get(), 5, entity.properties.dump());
                bind_text(database.get(), entity_statement.get(), 6, entity.extensions.dump());
                entity_statement.done();
                entity_statement.reset();
            }
            for (const auto& [id, asset] : revision.assets) {
                bind_revision(database.get(), asset_statement.get(), 1, revision.revision);
                bind_text(database.get(), asset_statement.get(), 2, id);
                bind_text(database.get(), asset_statement.get(), 3, asset.media_type);
                bind_text(database.get(), asset_statement.get(), 4, asset.sha256);
                bind_text(database.get(), asset_statement.get(), 5, asset.metadata.dump());
                const auto bind_result = asset.bytes.empty()
                                             ? sqlite3_bind_zeroblob64(asset_statement.get(), 6, 0)
                                             : sqlite3_bind_blob64(
                                                   asset_statement.get(), 6, asset.bytes.data(),
                                                   static_cast<sqlite3_uint64>(asset.bytes.size()),
                                                   SQLITE_TRANSIENT);
                if (bind_result != SQLITE_OK) {
                    sqlite_error(database.get(), "cannot bind asset bytes");
                }
                asset_statement.done();
                asset_statement.reset();
            }
        }

        Statement named_statement(database.get(),
                                  "INSERT INTO named_revisions(name,revision) VALUES(?1,?2)");
        for (const auto& [name, revision] : snapshot.named_revisions()) {
            bind_text(database.get(), named_statement.get(), 1, name);
            bind_revision(database.get(), named_statement.get(), 2, revision);
            named_statement.done();
            named_statement.reset();
        }
        execute(database.get(), "COMMIT");
        committed = true;
    } catch (...) {
        if (!committed) {
            sqlite3_exec(database.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        }
        throw;
    }
    if (sqlite3_db_cacheflush(database.get()) != SQLITE_OK) {
        sqlite_error(database.get(), "cannot flush SQLite page cache");
    }
    database.close();
    return requested_digest;
}

std::string required_metadata(sqlite3* database, std::string_view key) {
    Statement statement(database, "SELECT value FROM metadata WHERE key=?1");
    bind_text(database, statement.get(), 1, key);
    if (!statement.row()) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project metadata is missing " + std::string(key));
    }
    auto value = column_text(statement.get(), 0, kMaximumJsonBytes, key);
    if (statement.row()) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project metadata contains duplicate " + std::string(key));
    }
    return value;
}

Revision parse_revision_text(std::string_view value, std::string_view field) {
    Revision result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result > static_cast<Revision>(std::numeric_limits<sqlite3_int64>::max())) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string(field) + " is not a valid revision");
    }
    return result;
}

bool verify_sqlite_schema(sqlite3* database, bool allow_recovery = false) {
    Statement application(database, "PRAGMA application_id");
    if (!application.row() || sqlite3_column_type(application.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int(application.get(), 0) != kApplicationId || application.row()) {
        storage_error(StorageErrorCode::unsupported_format,
                      "file is not a Vertex project database");
    }
    Statement user_version(database, "PRAGMA user_version");
    if (!user_version.row() || sqlite3_column_type(user_version.get(), 0) != SQLITE_INTEGER ||
        (sqlite3_column_int(user_version.get(), 0) != 1 &&
         sqlite3_column_int(user_version.get(), 0) != 2 &&
         sqlite3_column_int(user_version.get(), 0) != 3 &&
         sqlite3_column_int(user_version.get(), 0) != 5 &&
         sqlite3_column_int(user_version.get(), 0) != 6 &&
         !(allow_recovery && sqlite3_column_int(user_version.get(), 0) == 4))) {
        storage_error(StorageErrorCode::unsupported_format,
                      "unsupported SQLite project user_version");
    }
    const bool translations = sqlite3_column_int(user_version.get(), 0) >= 5;
    const bool transforms = sqlite3_column_int(user_version.get(), 0) >= 6;
    const bool recovery = sqlite3_column_int(user_version.get(), 0) == 4 ||
        (translations && scalar_nonnegative(database,
            "SELECT count(*) FROM sqlite_schema WHERE name='project_recovery_records'",
            "recovery table count") != 0);
    if (recovery && !allow_recovery)
        storage_error(StorageErrorCode::unsupported_format,
                      "document-only load cannot discard an archive recovery ledger");
    if (user_version.row()) storage_error(StorageErrorCode::unsupported_format, "multiple format markers");
    Statement journal_mode(database, "PRAGMA journal_mode");
    if (!journal_mode.row() || column_text(journal_mode.get(), 0, 32, "journal_mode") != "delete" ||
        journal_mode.row()) {
        storage_error(StorageErrorCode::integrity_failure,
                      "standalone project uses an unsupported SQLite journal mode");
    }
    std::set<std::string, std::less<>> expected_schema{
        "metadata", "named_revisions", "revision_assets", "revision_entities", "revisions"};
    if (recovery) expected_schema.insert("project_recovery_records");
    if (scalar_nonnegative(
            database,
            "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'",
            "schema object count") != expected_schema.size()) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project schema does not have the exact format v1 object count");
    }
    std::set<std::string, std::less<>> actual_schema;
    Statement schema(database,
                     "SELECT name,type FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%' ORDER BY name");
    while (schema.row()) {
        const auto name = column_text(schema.get(), 0, 128, "schema name");
        if (column_text(schema.get(), 1, 16, "schema type") != "table" ||
            !actual_schema.insert(name).second) {
            storage_error(StorageErrorCode::integrity_failure,
                          "project contains an unsupported schema object");
        }
    }
    if (actual_schema != expected_schema) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project schema does not match format version 1");
    }

    struct ColumnSpec {
        std::string_view name;
        std::string_view type;
        int not_null;
        int primary_key;
    };
    std::map<std::string_view, std::vector<ColumnSpec>, std::less<>> expected_columns{
        {"metadata", {{"key", "TEXT", 1, 1}, {"value", "TEXT", 1, 0}}},
        {"revisions",
         {{"revision", "INTEGER", 0, 1},
          {"parent_revision", "INTEGER", 0, 0},
          {"source_revision", "INTEGER", 0, 0},
          {"action", "TEXT", 1, 0},
          {"name", "TEXT", 0, 0},
          {"undo_stack_json", "TEXT", 1, 0},
          {"redo_stack_json", "TEXT", 1, 0}}},
        {"revision_entities",
         {{"revision", "INTEGER", 1, 1},
          {"id", "TEXT", 1, 2},
          {"type", "TEXT", 1, 0},
          {"required", "INTEGER", 1, 0},
          {"properties_json", "TEXT", 1, 0},
          {"extensions_json", "TEXT", 1, 0}}},
        {"revision_assets",
         {{"revision", "INTEGER", 1, 1},
          {"asset_id", "TEXT", 1, 2},
          {"media_type", "TEXT", 1, 0},
          {"sha256", "TEXT", 1, 0},
          {"metadata_json", "TEXT", 1, 0},
          {"data", "BLOB", 1, 0}}},
        {"named_revisions", {{"name", "TEXT", 1, 1}, {"revision", "INTEGER", 1, 0}}},
    };
    if (translations) expected_columns.at("revisions").push_back({"boundary_translation_json", "TEXT", 0, 0});
    if (transforms) expected_columns.at("revisions").push_back({"boundary_transform_json", "TEXT", 0, 0});
    if (recovery) expected_columns.emplace("project_recovery_records", std::vector<ColumnSpec>{
        {"record_id", "TEXT", 1, 1}, {"record_kind", "TEXT", 1, 0}, {"envelope_json", "TEXT", 1, 0}});
    for (const auto& [table, columns] : expected_columns) {
        const std::string table_list_sql = "SELECT type,ncol,wr,strict FROM pragma_table_list WHERE "
                                           "schema='main' AND name='" +
                                           std::string(table) + "'";
        Statement table_list(database, table_list_sql.c_str());
        if (!table_list.row() || column_text(table_list.get(), 0, 16, "table type") != "table" ||
            sqlite3_column_int(table_list.get(), 1) != static_cast<int>(columns.size()) ||
            sqlite3_column_int(table_list.get(), 2) != 0 ||
            sqlite3_column_int(table_list.get(), 3) != 1 || table_list.row()) {
            storage_error(StorageErrorCode::integrity_failure,
                          "project table STRICT columns do not match format v1: " +
                              std::string(table));
        }
        const std::string columns_sql = "PRAGMA table_xinfo(" + std::string(table) + ")";
        Statement actual_columns(database, columns_sql.c_str());
        std::size_t index = 0;
        while (actual_columns.row()) {
            if (index >= columns.size() || sqlite3_column_int(actual_columns.get(), 0) != index ||
                column_text(actual_columns.get(), 1, 128, "column name") != columns[index].name ||
                column_text(actual_columns.get(), 2, 32, "column type") != columns[index].type ||
                sqlite3_column_int(actual_columns.get(), 3) != columns[index].not_null ||
                sqlite3_column_type(actual_columns.get(), 4) != SQLITE_NULL ||
                sqlite3_column_int(actual_columns.get(), 5) != columns[index].primary_key ||
                sqlite3_column_int(actual_columns.get(), 6) != 0) {
                storage_error(StorageErrorCode::integrity_failure,
                              "project table columns do not match format v1: " +
                                  std::string(table));
            }
            ++index;
        }
        if (index != columns.size()) {
            storage_error(StorageErrorCode::integrity_failure,
                          "project table columns do not match format v1: " +
                              std::string(table));
        }
    }

    std::map<std::string_view, std::set<std::string, std::less<>>, std::less<>>
        expected_foreign_keys{
            {"metadata", {}},
            {"revisions",
             {"parent_revision|revisions|revision|NO ACTION|NO ACTION|NONE",
              "source_revision|revisions|revision|NO ACTION|NO ACTION|NONE"}},
            {"revision_entities",
             {"revision|revisions|revision|NO ACTION|CASCADE|NONE"}},
            {"revision_assets", {"revision|revisions|revision|NO ACTION|CASCADE|NONE"}},
            {"named_revisions",
             {"revision|revisions|revision|NO ACTION|NO ACTION|NONE"}},
        };
    if (recovery) expected_foreign_keys.emplace("project_recovery_records", std::set<std::string, std::less<>>{});
    for (const auto& [table, expected] : expected_foreign_keys) {
        const std::string foreign_key_sql = "PRAGMA foreign_key_list(" + std::string(table) + ")";
        Statement foreign_key(database, foreign_key_sql.c_str());
        std::set<std::string, std::less<>> actual;
        while (foreign_key.row()) {
            if (sqlite3_column_int(foreign_key.get(), 1) != 0) {
                storage_error(StorageErrorCode::integrity_failure,
                              "project contains a composite foreign key outside format v1");
            }
            const auto signature = column_text(foreign_key.get(), 3, 128, "foreign key column") +
                                   "|" + column_text(foreign_key.get(), 2, 128, "foreign key table") +
                                   "|" + column_text(foreign_key.get(), 4, 128, "foreign key target") +
                                   "|" + column_text(foreign_key.get(), 5, 32, "foreign key update") +
                                   "|" + column_text(foreign_key.get(), 6, 32, "foreign key delete") +
                                   "|" + column_text(foreign_key.get(), 7, 32, "foreign key match");
            if (!actual.insert(signature).second) {
                storage_error(StorageErrorCode::integrity_failure,
                              "project contains duplicate foreign keys");
            }
        }
        if (actual != expected) {
            storage_error(StorageErrorCode::integrity_failure,
                          "project table foreign keys do not match format v1: " +
                              std::string(table));
        }
    }

    static const std::set<std::string, std::less<>> expected_metadata{
        "document_id", "format_version", "head_revision", "logical_digest", "saved_revision"};
    if (scalar_nonnegative(database, "SELECT count(*) FROM metadata", "metadata row count") !=
        expected_metadata.size()) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project metadata key set does not match format v1");
    }
    std::set<std::string, std::less<>> actual_metadata;
    Statement metadata_keys(database, "SELECT key FROM metadata ORDER BY key");
    while (metadata_keys.row()) {
        actual_metadata.insert(column_text(metadata_keys.get(), 0, 128, "metadata key"));
    }
    if (actual_metadata != expected_metadata) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project metadata key set does not match format v1");
    }
    return recovery;
}

void verify_sqlite_content_integrity(sqlite3* database) {
    Statement integrity(database, "PRAGMA integrity_check");
    std::size_t rows = 0;
    while (integrity.row()) {
        ++rows;
        if (column_text(integrity.get(), 0, 4096, "integrity_check") != "ok") {
            storage_error(StorageErrorCode::integrity_failure,
                          "SQLite integrity check rejected the project");
        }
    }
    if (rows != 1) {
        storage_error(StorageErrorCode::integrity_failure,
                      "SQLite integrity check returned an unexpected result");
    }
    Statement foreign_keys(database, "PRAGMA foreign_key_check");
    if (foreign_keys.row()) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project contains a broken database reference");
    }
}

DocumentSnapshot read_snapshot(sqlite3* database, RecoveryLedger* recovery = nullptr,
                               std::string* verified_digest = nullptr) {
    const auto format = required_metadata(database, "format_version");
    if (format != "1" && format != "2" && format != "3" && format != "5" && format != "6" && !(recovery && format == "4")) {
        storage_error(StorageErrorCode::unsupported_format,
                      "unsupported project format version: " + format);
    }
    const auto format_number = format == "6" ? 6U : (format == "5" ? 5U : (format == "4" ? 4U : (format == "3" ? 3U : (format == "2" ? 2U : 1U))));
    Statement format_marker(database, "PRAGMA user_version");
    if (!format_marker.row() ||
        sqlite3_column_int(format_marker.get(), 0) != static_cast<int>(format_number)) {
        storage_error(StorageErrorCode::unsupported_format, "project format markers disagree");
    }
    auto snapshot = ProjectStoreAccess::make_snapshot();
    const auto document_id = required_metadata(database, "document_id");
    const auto head_revision = parse_revision_text(required_metadata(database, "head_revision"),
                                                   "head_revision");
    const auto saved_text = required_metadata(database, "saved_revision");
    const std::optional<Revision> stored_saved = recovery && saved_text == "null"
        ? std::nullopt : std::optional<Revision>(parse_revision_text(saved_text, "saved_revision"));
    if (!recovery && stored_saved != head_revision) {
        storage_error(StorageErrorCode::integrity_failure,
                      "standalone project saved revision does not equal its head revision");
    }
    ProjectStoreAccess::set_identity(snapshot, document_id, head_revision, stored_saved);
    const auto expected_digest = required_metadata(database, "logical_digest");
    const auto counts = enforce_preallocation_budgets(database, recovery != nullptr);
    ProjectStoreAccess::reserve_history(snapshot, static_cast<std::size_t>(counts.revisions));
    DecodeBudget decode_budget(recovery != nullptr || format_number >= 5);

    Statement revisions(database,
        format_number >= 6
            ? "SELECT revision,parent_revision,source_revision,action,name,"
              "undo_stack_json,redo_stack_json,boundary_translation_json,boundary_transform_json FROM revisions ORDER BY revision"
            : format_number == 5
            ? "SELECT revision,parent_revision,source_revision,action,name,"
              "undo_stack_json,redo_stack_json,boundary_translation_json FROM revisions ORDER BY revision"
            : "SELECT revision,parent_revision,source_revision,action,name,"
              "undo_stack_json,redo_stack_json FROM revisions ORDER BY revision");
    while (revisions.row()) {
        auto& history = ProjectStoreAccess::history(snapshot);
        if (history.size() >= static_cast<std::size_t>(ProjectStore::maximum_revision_count)) {
            storage_error(StorageErrorCode::integrity_failure, "project has too many revisions");
        }
        RevisionRecord record;
        record.revision = column_revision(revisions.get(), 0, "revision");
        record.parent_revision = column_optional_revision(revisions.get(), 1, "parent_revision");
        record.source_revision = column_optional_revision(revisions.get(), 2, "source_revision");
        record.action = column_text(revisions.get(), 3, 1024, "action");
        if (sqlite3_column_type(revisions.get(), 4) != SQLITE_NULL) {
            record.name = column_text(revisions.get(), 4, 256, "name");
        }
        record.undo_stack = parse_revision_stack(
            column_text(revisions.get(), 5, kMaximumJsonBytes, "undo_stack_json"),
            "undo_stack_json", decode_budget);
        record.redo_stack = parse_revision_stack(
            column_text(revisions.get(), 6, kMaximumJsonBytes, "redo_stack_json"),
            "redo_stack_json", decode_budget);
        if (format_number >= 5 && sqlite3_column_type(revisions.get(), 7) != SQLITE_NULL) {
            const auto proof = parse_budgeted_json(
                column_text(revisions.get(), 7, 1024, "boundary_translation_json"), true,
                "boundary_translation_json", decode_budget);
            try { record.boundary_translation = decode_boundary_translation(proof); }
            catch (const std::exception& error) {
                storage_error(StorageErrorCode::integrity_failure, error.what());
            }
        }
        if (format_number >= 6 && sqlite3_column_type(revisions.get(), 8) != SQLITE_NULL) {
            const auto proof = parse_budgeted_json(
                column_text(revisions.get(), 8, 2048, "boundary_transform_json"), true,
                "boundary_transform_json", decode_budget);
            try { record.boundary_transform = decode_boundary_transform(proof); }
            catch (const std::exception& error) {
                storage_error(StorageErrorCode::integrity_failure, error.what());
            }
        }
        history.push_back(std::move(record));
    }
    if (ProjectStoreAccess::history(snapshot).empty() ||
        snapshot.revision() >= ProjectStoreAccess::history(snapshot).size()) {
        storage_error(StorageErrorCode::integrity_failure, "project head revision is absent");
    }

    Statement entities(database,
                       "SELECT revision,id,type,required,properties_json,extensions_json "
                       "FROM revision_entities ORDER BY revision,id");
    sqlite3_int64 entity_rows = 0;
    while (entities.row()) {
        if (++entity_rows > static_cast<sqlite3_int64>(ProjectStore::maximum_entity_rows)) {
            storage_error(StorageErrorCode::integrity_failure, "project has too many entity rows");
        }
        const auto revision = column_revision(entities.get(), 0, "entity revision");
        if (revision >= ProjectStoreAccess::history(snapshot).size()) {
            storage_error(StorageErrorCode::integrity_failure, "entity references an absent revision");
        }
        Entity entity;
        entity.id = column_text(entities.get(), 1, 128, "entity id");
        entity.type = column_text(entities.get(), 2, 64, "entity type");
        const auto required = column_revision(entities.get(), 3, "entity required flag");
        if (required > 1) {
            storage_error(StorageErrorCode::integrity_failure, "entity required flag is invalid");
        }
        entity.required = required == 1;
        entity.properties = parse_budgeted_json(
            column_text(entities.get(), 4, kMaximumJsonBytes, "properties_json"), true,
            "properties_json", decode_budget);
        entity.extensions = parse_budgeted_json(
            column_text(entities.get(), 5, kMaximumJsonBytes, "extensions_json"), true,
            "extensions_json", decode_budget);
        auto& target = ProjectStoreAccess::history(snapshot)[static_cast<std::size_t>(revision)].entities;
        if (!target.emplace(entity.id, std::move(entity)).second) {
            storage_error(StorageErrorCode::integrity_failure, "project contains duplicate entity ids");
        }
    }

    Statement assets(database,
                     "SELECT revision,asset_id,media_type,sha256,metadata_json,data "
                     "FROM revision_assets ORDER BY revision,asset_id");
    sqlite3_int64 asset_rows = 0;
    while (assets.row()) {
        if (++asset_rows > static_cast<sqlite3_int64>(ProjectStore::maximum_asset_rows)) {
            storage_error(StorageErrorCode::integrity_failure, "project has too many asset rows");
        }
        const auto revision = column_revision(assets.get(), 0, "asset revision");
        if (revision >= ProjectStoreAccess::history(snapshot).size()) {
            storage_error(StorageErrorCode::integrity_failure, "asset references an absent revision");
        }
        Asset asset;
        asset.id = column_text(assets.get(), 1, 128, "asset id");
        asset.media_type = column_text(assets.get(), 2, 256, "asset media type");
        asset.sha256 = column_text(assets.get(), 3, 64, "asset sha256");
        asset.metadata = parse_budgeted_json(
            column_text(assets.get(), 4, kMaximumJsonBytes, "asset metadata"), true,
            "asset metadata", decode_budget);
        if (sqlite3_column_type(assets.get(), 5) != SQLITE_BLOB) {
            storage_error(StorageErrorCode::integrity_failure, "asset data is not a SQLite blob");
        }
        const auto size = sqlite3_column_bytes(assets.get(), 5);
        if (size < 0 || size > kMaximumAssetBytes) {
            storage_error(StorageErrorCode::integrity_failure, "asset data exceeds its size limit");
        }
        const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(assets.get(), 5));
        if (size != 0 && data == nullptr) {
            storage_error(StorageErrorCode::integrity_failure, "asset data could not be read");
        }
        if (size != 0) {
            asset.bytes.assign(data, data + static_cast<std::size_t>(size));
        }
        if (sha256_hex(asset.bytes) != asset.sha256) {
            storage_error(StorageErrorCode::integrity_failure,
                          "asset SHA-256 does not match stored bytes: " + asset.id);
        }
        auto& target = ProjectStoreAccess::history(snapshot)[static_cast<std::size_t>(revision)].assets;
        if (!target.emplace(asset.id, std::move(asset)).second) {
            storage_error(StorageErrorCode::integrity_failure, "project contains duplicate asset ids");
        }
    }

    Statement names(database, "SELECT name,revision FROM named_revisions ORDER BY name");
    while (names.row()) {
        const auto name = column_text(names.get(), 0, 256, "revision name");
        const auto revision = column_revision(names.get(), 1, "named revision");
        if (!ProjectStoreAccess::names(snapshot).emplace(name, revision).second) {
            storage_error(StorageErrorCode::integrity_failure,
                          "project contains duplicate named revisions");
        }
    }

    const auto required_format = ProjectStore::required_format_version(snapshot);
    if (required_format > format_number) {
        const auto reason = required_format >= 6 ? "boundary transform" : required_format >= 5 ? "boundary translation" : required_format >= 3 ? "boundary_authoring" :
                            "identified boundary, dimension or boundary draft";
        storage_error(StorageErrorCode::unsupported_format,
                      "retained " + std::string(reason) + " history requires project format v" +
                          std::to_string(required_format));
    }
    if (recovery) {
        decode_budget.add_value(); // Enclosing ledger array.
        Statement rows(database, "SELECT record_id,record_kind,envelope_json FROM project_recovery_records ORDER BY record_id");
        while (rows.row()) {
            for (int overhead = 0; overhead < 6; ++overhead) decode_budget.add_value();
            RecoveryRecord row;
            row.record_id = column_text(rows.get(), 0, 128, "record_id");
            row.record_kind = column_text(rows.get(), 1, 128, "record_kind");
            decode_budget.add_string(29 + row.record_id.size() + row.record_kind.size());
            row.envelope = parse_budgeted_json(column_text(rows.get(), 2,
                ProjectStore::maximum_encoded_json_bytes, "envelope_json"), false, "envelope_json", decode_budget);
            recovery->push_back(std::move(row));
        }
        try { (void)preflight_recovery_ledger(snapshot, *recovery); }
        catch (const std::invalid_argument& error) {
            storage_error(StorageErrorCode::resource_limit, error.what());
        }
    }
    const auto decoded_digest = logical_digest(snapshot, format_number, recovery);
    if (decoded_digest != expected_digest) {
        storage_error(StorageErrorCode::integrity_failure,
                      "project logical SHA-256 does not match its stored content");
    }
    if (verified_digest) *verified_digest = decoded_digest;
    return snapshot;
}

void validate_project_path(const std::filesystem::path& path, bool require_existing) {
    if (path.empty() || path.filename().empty() || path.filename() == "." || path.filename() == "..") {
        storage_error(StorageErrorCode::io_error, "project path does not name a file");
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        storage_error(StorageErrorCode::io_error, "cannot inspect project path: " + error.message());
    }
    if (std::filesystem::is_symlink(status) || std::filesystem::is_directory(status)) {
        storage_error(StorageErrorCode::io_error, "project path cannot be a link or directory");
    }
    if (require_existing && !std::filesystem::is_regular_file(status)) {
        storage_error(StorageErrorCode::io_error, "project file does not exist");
    }
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
    if (!std::filesystem::is_directory(parent, error) || error) {
        storage_error(StorageErrorCode::io_error, "project parent directory does not exist");
    }
#ifdef _WIN32
    if (invalid_standalone_filename(path.filename().wstring())) {
        storage_error(StorageErrorCode::io_error,
                      "project path must name a standalone Windows file, not a device or stream");
    }
    const auto attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        storage_error(StorageErrorCode::io_error,
                      "project parent directory cannot be a Windows reparse point");
    }
#endif
}

void validate_durable_destination(const std::filesystem::path& destination) {
#ifdef _WIN32
    std::error_code path_error;
    const auto parent = std::filesystem::absolute(
        destination.has_parent_path() ? destination.parent_path()
                                      : std::filesystem::current_path(),
        path_error);
    if (path_error) {
        storage_error(StorageErrorCode::io_error,
                      "cannot resolve project destination volume");
    }
    std::vector<wchar_t> volume_root(32768, L'\0');
    if (!GetVolumePathNameW(parent.c_str(), volume_root.data(),
                            static_cast<DWORD>(volume_root.size()))) {
        storage_error(StorageErrorCode::io_error,
                      "cannot resolve project destination volume, Windows error " +
                          std::to_string(GetLastError()));
    }
    if (GetDriveTypeW(volume_root.data()) != DRIVE_FIXED) {
        storage_error(StorageErrorCode::io_error,
                      "durable project save requires a local fixed disk");
    }
    std::array<wchar_t, 64> file_system{};
    if (!GetVolumeInformationW(volume_root.data(), nullptr, 0, nullptr, nullptr, nullptr,
                               file_system.data(), static_cast<DWORD>(file_system.size()))) {
        storage_error(StorageErrorCode::io_error,
                      "cannot identify project destination filesystem, Windows error " +
                          std::to_string(GetLastError()));
    }
    if (_wcsicmp(file_system.data(), L"NTFS") != 0 &&
        _wcsicmp(file_system.data(), L"ReFS") != 0) {
        storage_error(StorageErrorCode::io_error,
                      "durable project save requires NTFS or ReFS");
    }
#else
    (void)destination;
    storage_error(StorageErrorCode::io_error,
                  "durable project save requires local Windows NTFS or ReFS");
#endif
}

std::filesystem::path sqlite_sidecar(const std::filesystem::path& path,
                                     std::string_view suffix) {
    return std::filesystem::path(path.native() + std::filesystem::path(suffix).native());
}

#ifdef _WIN32
void copy_handle_to_new_file(HANDLE source, const std::filesystem::path& destination) {
    const auto target = CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (target == INVALID_HANDLE_VALUE) {
        storage_error(StorageErrorCode::io_error,
                      "cannot create project backup, Windows error " +
                          std::to_string(GetLastError()));
    }
    try {
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN)) {
            storage_error(StorageErrorCode::io_error,
                          "cannot seek source project for backup");
        }
        std::vector<unsigned char> buffer(1024 * 1024);
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                          nullptr)) {
                storage_error(StorageErrorCode::io_error,
                              "cannot read source project for backup, Windows error " +
                                  std::to_string(GetLastError()));
            }
            if (read == 0) {
                break;
            }
            DWORD offset = 0;
            while (offset < read) {
                DWORD written = 0;
                if (!WriteFile(target, buffer.data() + offset, read - offset, &written, nullptr) ||
                    written == 0) {
                    storage_error(StorageErrorCode::io_error,
                                  "cannot write complete project backup, Windows error " +
                                      std::to_string(GetLastError()));
                }
                offset += written;
            }
        }
        if (!FlushFileBuffers(target)) {
            storage_error(StorageErrorCode::io_error,
                          "cannot flush project backup, Windows error " +
                              std::to_string(GetLastError()));
        }
    } catch (...) {
        CloseHandle(target);
        throw;
    }
    CloseHandle(target);
}

void publish_handle(HANDLE staging, const std::filesystem::path& destination,
                    bool replace_existing) {
    std::error_code absolute_error;
    const auto absolute = std::filesystem::absolute(destination, absolute_error);
    if (absolute_error) {
        storage_error(StorageErrorCode::io_error,
                      "cannot resolve project publication path: " + absolute_error.message());
    }
    const auto name = absolute.wstring();
    const auto name_bytes = name.size() * sizeof(wchar_t);
    if (name_bytes > static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) -
                         offsetof(FILE_RENAME_INFO, FileName)) {
        storage_error(StorageErrorCode::resource_limit,
                      "project publication path exceeds the Windows rename limit");
    }
    // FILE_RENAME_INFO has a one-element trailing FileName member. Windows' documented
    // allocation pattern is sizeof(FILE_RENAME_INFO) plus the non-NUL-terminated name bytes.
    const auto information_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    std::vector<unsigned char> storage(information_size, 0);
    auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    information->Flags = replace_existing
                             ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                                   FILE_RENAME_FLAG_POSIX_SEMANTICS
                             : 0;
    information->RootDirectory = nullptr;
    information->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(information->FileName, name.data(), name_bytes);
    if (!SetFileInformationByHandle(staging, FileRenameInfoEx, information,
                                    static_cast<DWORD>(information_size))) {
        storage_error(StorageErrorCode::io_error,
                      "Windows handle-based project publication failed with error " +
                          std::to_string(GetLastError()));
    }
}
#endif

std::filesystem::path sibling_path(const std::filesystem::path& destination,
                                   std::string_view category) {
    // Keep private publication names independent of the user-selected filename.
    // SQLite may append -journal/-wal/-shm while writing the staging database;
    // repeating a long destination filename here can push those internal paths
    // beyond the legacy Win32 path boundary even though the destination itself
    // is valid.
    const auto name = L".vertex-project-" + std::filesystem::path(category).wstring() + L"-" +
                      std::filesystem::path(make_stable_id()).wstring();
    return destination.parent_path() / name;
}

class TemporaryFile final {
public:
    explicit TemporaryFile(std::filesystem::path path, bool include_sqlite_sidecars = true)
        : path_(std::move(path)), include_sqlite_sidecars_(include_sqlite_sidecars) {}
    ~TemporaryFile() {
        if (active_) {
            if (include_sqlite_sidecars_) {
                for (const auto* suffix : {"-journal", "-wal", "-shm"}) {
                    std::error_code ignored;
                    std::filesystem::remove(sqlite_sidecar(path_, suffix), ignored);
                }
            }
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    [[nodiscard]] std::vector<std::filesystem::path> cleanup_checked() {
        std::vector<std::filesystem::path> residuals;
        auto remove_one = [&](const std::filesystem::path& candidate) {
            std::error_code remove_error;
            const bool removed = std::filesystem::remove(candidate, remove_error);
            std::error_code exists_error;
            const bool remains = std::filesystem::exists(candidate, exists_error);
            if (remove_error || exists_error || (!removed && remains)) {
                residuals.push_back(candidate);
            }
        };
        if (include_sqlite_sidecars_) {
            for (const auto* suffix : {"-journal", "-wal", "-shm"}) {
                remove_one(sqlite_sidecar(path_, suffix));
            }
        }
        remove_one(path_);
        // A reported residue is deliberately left identified for the caller. Do not let the
        // noexcept destructor make a later, silent attempt with different observable results.
        active_ = false;
        return residuals;
    }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool include_sqlite_sidecars_ = true;
    bool active_ = true;
};

void inject_if(SaveFaultStage configured, SaveFaultStage stage) {
    if (configured == stage) {
        storage_error(StorageErrorCode::injected_failure,
                      "project save stopped at the requested fault-injection stage");
    }
}

void append_unique_residuals(std::vector<std::filesystem::path>& destination,
                             std::vector<std::filesystem::path> source) {
    for (auto& path : source) {
        if (std::find(destination.begin(), destination.end(), path) == destination.end()) {
            destination.push_back(std::move(path));
        }
    }
}

std::string residual_diagnostic(const std::vector<std::filesystem::path>& residuals) {
    std::string result = "; checked cleanup left residual paths:";
    for (const auto& path : residuals) {
        result += " ";
        result += path_utf8(path);
    }
    return result;
}

#ifdef _WIN32
bool refuse_recovery_destination_for_document_save(HANDLE source) {
    // SQLite's fixed header stores the big-endian user_version at byte 60.
    // Read the already hash-checked object, not a separately resolved pathname.
    std::array<unsigned char, 100> header{};
    LARGE_INTEGER beginning{};
    DWORD count = 0;
    if (!SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN) ||
        !ReadFile(source, header.data(), static_cast<DWORD>(header.size()), &count, nullptr)) {
        storage_error(StorageErrorCode::io_error,
                      "cannot inspect destination format before document-only save");
    }
    constexpr char sqlite_header[] = "SQLite format 3";
    if (count < header.size() ||
        std::memcmp(header.data(), sqlite_header, sizeof(sqlite_header)) != 0) {
        return false;
    }
    const auto version = (static_cast<std::uint32_t>(header[60]) << 24) |
                         (static_cast<std::uint32_t>(header[61]) << 16) |
                         (static_cast<std::uint32_t>(header[62]) << 8) |
                         static_cast<std::uint32_t>(header[63]);
    if (version == 4 || version > 6) {
        storage_error(StorageErrorCode::unsupported_format,
                      "document-only save cannot replace a recovery or future-format archive");
    }
    // A v5 or v6 file can be either a document or an archive. Its hash-verified
    // backup must be decoded before publication to distinguish them safely.
    return version == 5 || version == 6;
}

std::string hash_handle_contents(HANDLE source) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    auto check = [](NTSTATUS status, std::string_view operation) {
        if (status < 0) {
            storage_error(StorageErrorCode::io_error,
                          std::string("BCrypt file SHA-256 ") + std::string(operation) + " failed");
        }
    };
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
              "initialization");
        check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                &received, 0),
              "object-size query");
        check(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &received,
                                0),
              "digest-size query");
        object.resize(object_size);
        digest.resize(hash_size);
        check(BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0),
              "hash creation");
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN)) {
            storage_error(StorageErrorCode::io_error,
                          "cannot seek project handle for SHA-256, Windows error " +
                              std::to_string(GetLastError()));
        }
        std::vector<unsigned char> buffer(1024 * 1024);
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                          nullptr)) {
                storage_error(StorageErrorCode::io_error,
                              "cannot read complete project handle for SHA-256, Windows error " +
                                  std::to_string(GetLastError()));
            }
            if (count == 0) {
                break;
            }
            check(BCryptHashData(hash, buffer.data(), count, 0), "update");
        }
        check(BCryptFinishHash(hash, digest.data(), hash_size, 0), "finalization");
    } catch (...) {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = hex[digest[index] >> 4U];
        result[index * 2 + 1] = hex[digest[index] & 0x0fU];
    }
    return result;
}

struct DecodedProject {
    DocumentSnapshot snapshot;
    std::string file_sha256;
    std::string logical_digest;
    RecoveryLedger ledger;
    RecoveryLedgerDecodeResult recovery;
};

DecodedProject decode_project_under_lock(const std::filesystem::path& source, HANDLE locked_file,
                                         std::optional<ArchiveRole> role = std::nullopt) {
    auto database = open_database(source, SQLITE_OPEN_READONLY);
    execute(database.get(), "PRAGMA trusted_schema=OFF");
    execute(database.get(), "PRAGMA query_only=ON");
    execute(database.get(), "PRAGMA foreign_keys=ON");
    const bool is_archive = verify_sqlite_schema(database.get(), role.has_value());
    (void)enforce_preallocation_budgets(database.get(), is_archive);
    verify_sqlite_content_integrity(database.get());
    RecoveryLedger ledger;
    std::string verified_digest;
    auto snapshot = read_snapshot(database.get(), is_archive ? &ledger : nullptr, &verified_digest);
    RecoveryLedgerDecodeResult recovery;
    if (is_archive) {
        try {
            recovery = decode_recovery_ledger(snapshot, ledger, *role);
        snapshot = Document::fork(snapshot).snapshot();
        } catch (const DocumentError& error) {
            storage_error(StorageErrorCode::integrity_failure,
                          std::string("archive document validation failed: ") + error.what());
        } catch (const std::invalid_argument& error) {
            storage_error(StorageErrorCode::integrity_failure, error.what());
        }
    }
    database.close();
    return DecodedProject{std::move(snapshot), hash_handle_contents(locked_file), std::move(verified_digest),
        std::move(ledger), std::move(recovery)};
}
#endif

}  // namespace

StorageError::StorageError(StorageErrorCode code, std::string message,
                           std::vector<std::filesystem::path> residual_paths)
    : std::runtime_error(std::move(message)),
      code_(code),
      residual_paths_(std::move(residual_paths)) {}

StorageErrorCode StorageError::code() const noexcept { return code_; }

const std::vector<std::filesystem::path>& StorageError::residual_paths() const noexcept {
    return residual_paths_;
}

std::string ProjectStore::file_sha256(const std::filesystem::path& source) {
    try {
        validate_project_path(source, true);
#ifdef _WIN32
        const LockedReadFile lock(source);
        return hash_handle_contents(lock.get());
#else
        storage_error(StorageErrorCode::io_error, "project SHA-256 requires Windows BCrypt");
#endif
    } catch (const StorageError&) {
        throw;
    } catch (const std::bad_alloc&) {
        storage_error(StorageErrorCode::resource_limit,
                      "insufficient memory to fingerprint project within v1 limits");
    } catch (const std::filesystem::filesystem_error& error) {
        storage_error(StorageErrorCode::io_error,
                      std::string("project fingerprint filesystem error: ") + error.what());
    }
}

LoadResult ProjectStore::load(const std::filesystem::path& source) {
    try {
        validate_project_path(source, true);
        for (const auto* suffix : {"-journal", "-wal", "-shm"}) {
            const auto sidecar = sqlite_sidecar(source, suffix);
            std::error_code sidecar_error;
            if (std::filesystem::exists(sidecar, sidecar_error) || sidecar_error) {
                storage_error(StorageErrorCode::integrity_failure,
                              "standalone project has an unexpected SQLite sidecar file");
            }
        }
#ifdef _WIN32
        const LockedReadFile lock(source);
        auto decoded = decode_project_under_lock(source, lock.get());
        auto document = Document::restore(std::move(decoded.snapshot));
        return LoadResult{std::move(document), std::move(decoded.file_sha256)};
#else
        storage_error(StorageErrorCode::io_error,
                      "project load requires Windows locked-file semantics");
#endif
    } catch (const DocumentError& error) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string("project document validation failed: ") + error.what());
    } catch (const StorageError&) {
        throw;
    } catch (const std::bad_alloc&) {
        storage_error(StorageErrorCode::resource_limit,
                      "insufficient memory to decode project within v1 limits");
    } catch (const std::length_error&) {
        storage_error(StorageErrorCode::resource_limit,
                      "project requested an invalid allocation size");
    } catch (const nlohmann::json::exception& error) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string("project contains JSON that cannot be preserved: ") +
                          error.what());
    } catch (const std::filesystem::filesystem_error& error) {
        storage_error(StorageErrorCode::io_error,
                      std::string("project load filesystem error: ") + error.what());
    }
}

namespace {
SaveReceipt save_project(const std::filesystem::path& destination,
                         const DocumentSnapshot& snapshot, const SaveOptions& options,
                         const ProjectArchiveSnapshot* archive = nullptr) {
    try {
        validate_project_path(destination, false);
        validate_durable_destination(destination);
#ifdef _WIN32
        const DestinationSaveMutex save_mutex(destination);
#endif
        if (archive) {
            try {
                const auto decoded = decode_recovery_ledger(snapshot, archive->recovery(), archive->role());
                if (!decoded.supported())
                    storage_error(StorageErrorCode::unsupported_format,
                                  "cannot save an opaque or role-mismatched recovery archive");
            } catch (const std::invalid_argument& error) {
                storage_error(StorageErrorCode::invalid_snapshot, error.what());
            }
        }
        // Bound caller-owned data before any I/O. Complete structural validation
        // runs against the decoded staging file below, so the exact bytes to be
        // published are validated once without first copying the whole snapshot.
        enforce_snapshot_budget(snapshot);

        std::error_code path_error;
        const bool destination_exists = std::filesystem::exists(destination, path_error);
        if (path_error) {
            storage_error(StorageErrorCode::io_error,
                          "cannot inspect save destination: " + path_error.message());
        }
        if (destination_exists && !options.expected_destination_sha256.has_value()) {
            storage_error(StorageErrorCode::destination_exists,
                          "existing project requires its expected SHA-256 fingerprint");
        }
        if (!destination_exists && options.expected_destination_sha256.has_value()) {
            storage_error(StorageErrorCode::external_change,
                          "expected project destination disappeared before save");
        }
        if (destination_exists &&
            ProjectStore::file_sha256(destination) != *options.expected_destination_sha256) {
            storage_error(StorageErrorCode::external_change,
                          "project destination changed outside this document session");
        }

        const auto temporary = sibling_path(destination, "tmp");
        TemporaryFile temporary_cleanup(temporary);
        std::optional<std::filesystem::path> backup;
        std::unique_ptr<TemporaryFile> backup_cleanup;
#ifdef _WIN32
        std::unique_ptr<DestinationPublicationGuard> destination_guard;
        std::unique_ptr<LockedReadFile> backup_guard;
#endif
        try {
            if (std::filesystem::exists(temporary)) {
                storage_error(StorageErrorCode::io_error,
                              "temporary project path unexpectedly exists");
            }
#ifdef _WIN32
            ReservedStagingFile reserved_staging(temporary);
            const auto requested_digest = write_database(
                temporary, snapshot, options.fault_stage, archive ? &archive->recovery() : nullptr);
            inject_if(options.fault_stage, SaveFaultStage::after_database_write);
            reserved_staging.flush_written_bytes();

            const auto staging_validation = reserved_staging.open_validation_handle();
            StagedPublicationFile staging(reserved_staging.release_identity_handle(),
                                          staging_validation);
            for (const auto* suffix : {"-journal", "-wal", "-shm"}) {
                const auto sidecar = sqlite_sidecar(temporary, suffix);
                std::error_code sidecar_error;
                if (std::filesystem::exists(sidecar, sidecar_error) || sidecar_error) {
                    storage_error(StorageErrorCode::integrity_failure,
                                  "validated staging project has an unexpected SQLite sidecar");
                }
            }
            auto validation = decode_project_under_lock(temporary, staging.validation_handle(),
                archive ? std::optional<ArchiveRole>(archive->role()) : std::nullopt);
            if (archive && !validation.recovery.supported())
                storage_error(StorageErrorCode::integrity_failure, "staged recovery archive is not supported");
            // Both digests were already needed to write and verify the staging
            // file. Reuse them instead of rebuilding two full logical manifests.
            if (validation.logical_digest != requested_digest) {
                storage_error(StorageErrorCode::integrity_failure,
                              "validated project content differs from requested snapshot");
            }
            Document validated_document = [&]() {
                try {
                    return ProjectStoreAccess::restore_document(std::move(validation.snapshot));
                } catch (const DocumentError& error) {
                    storage_error(StorageErrorCode::invalid_snapshot,
                                  std::string("cannot save invalid document snapshot: ") + error.what());
                }
            }();
            if (validated_document.revision() != snapshot.revision()) {
                storage_error(StorageErrorCode::integrity_failure,
                              "validated project revision differs from requested snapshot");
            }
            const auto publication_handle = staging.acquire_publication_handle();
            const auto publication_digest = hash_handle_contents(publication_handle);
            if (publication_digest != validation.file_sha256) {
                storage_error(StorageErrorCode::integrity_failure,
                              "staging bytes changed after complete validation");
            }
            if (options.after_validation_barrier) {
                options.after_validation_barrier(temporary, publication_digest);
            }
            inject_if(options.fault_stage, SaveFaultStage::after_validation);
            if (destination_exists) {
                destination_guard =
                    std::make_unique<DestinationPublicationGuard>(destination);
                const auto current_digest =
                    hash_handle_contents(destination_guard->identity_handle());
                if (current_digest != *options.expected_destination_sha256) {
                    storage_error(StorageErrorCode::external_change,
                                  "project destination changed during save validation");
                }
                const bool verify_v5_document = !archive &&
                    refuse_recovery_destination_for_document_save(destination_guard->identity_handle());
                backup = sibling_path(destination, "bak");
                backup_cleanup = std::make_unique<TemporaryFile>(*backup, false);
                copy_handle_to_new_file(destination_guard->identity_handle(), *backup);
                backup_guard = std::make_unique<LockedReadFile>(*backup);
                if (hash_handle_contents(backup_guard->get()) !=
                    *options.expected_destination_sha256) {
                    storage_error(StorageErrorCode::integrity_failure,
                                  "flushed project backup differs from expected destination");
                }
                if (verify_v5_document)
                    (void)decode_project_under_lock(*backup, backup_guard->get());
                if (archive) {
                    // Decode the hash-verified copy of the locked destination object,
                    // avoiding a pathname re-resolution race on the replaceable name.
                    auto existing = decode_project_under_lock(*backup, backup_guard->get(), archive->role());
                    if (!existing.ledger.empty() && !existing.recovery.supported())
                        storage_error(StorageErrorCode::unsupported_format,
                                      "cannot replace an opaque or differently owned archive role");
                }
            }

            if (options.before_publication_barrier) {
                options.before_publication_barrier(
                    destination,
                    destination_exists ? *options.expected_destination_sha256 : std::string{},
                    backup);
            }
            inject_if(options.fault_stage, SaveFaultStage::before_publish);

            // A path-level Windows compare-and-swap does not exist. Keep the expected object
            // read-locked against writes, then hold a second matching-name handle across the
            // handle-based replacement. Rename/delete-only interference still fails closed when
            // it occurs before this last identity check.
            if (destination_exists) {
                const auto current_name =
                    destination_guard->lock_and_match_current_name(destination);
                if (hash_handle_contents(current_name) !=
                    *options.expected_destination_sha256) {
                    storage_error(StorageErrorCode::external_change,
                                  "project destination changed immediately before publication");
                }
            } else {
                const bool still_exists = std::filesystem::exists(destination, path_error);
                if (path_error || still_exists) {
                    storage_error(StorageErrorCode::external_change,
                                  "project destination appeared during save validation");
                }
            }

            SaveReceipt receipt{snapshot.revision(), publication_digest, backup};
            publish_handle(publication_handle, destination, destination_exists);
            temporary_cleanup.release();
            if (backup_cleanup) {
                backup_cleanup->release();
            }
            return receipt;
#else
            storage_error(StorageErrorCode::io_error,
                          "atomic project publication requires Windows");
#endif
        } catch (const StorageError& original) {
#ifdef _WIN32
            backup_guard.reset();
#endif
            std::vector<std::filesystem::path> cleanup_residuals;
            if (backup_cleanup) {
                append_unique_residuals(cleanup_residuals,
                                        backup_cleanup->cleanup_checked());
            }
            append_unique_residuals(cleanup_residuals,
                                    temporary_cleanup.cleanup_checked());
            if (!cleanup_residuals.empty()) {
                auto all_residuals = original.residual_paths();
                append_unique_residuals(all_residuals, std::move(cleanup_residuals));
                auto message = std::string(original.what()) + residual_diagnostic(all_residuals);
                throw StorageError(original.code(),
                                   std::move(message), std::move(all_residuals));
            }
            throw;
        } catch (...) {
            const auto original = std::current_exception();
#ifdef _WIN32
            backup_guard.reset();
#endif
            std::vector<std::filesystem::path> residuals;
            if (backup_cleanup) {
                append_unique_residuals(residuals, backup_cleanup->cleanup_checked());
            }
            append_unique_residuals(residuals, temporary_cleanup.cleanup_checked());
            if (!residuals.empty()) {
                std::string message = "project save failed";
                try {
                    std::rethrow_exception(original);
                } catch (const std::exception& error) {
                    message += ": ";
                    message += error.what();
                } catch (...) {
                }
                message += residual_diagnostic(residuals);
                throw StorageError(StorageErrorCode::io_error,
                                   std::move(message), std::move(residuals));
            }
            throw;
        }
    } catch (const StorageError&) {
        throw;
    } catch (const std::bad_alloc&) {
        storage_error(StorageErrorCode::resource_limit,
                      "insufficient memory to save project within v1 limits");
    } catch (const std::length_error&) {
        storage_error(StorageErrorCode::resource_limit,
                      "project save requested an invalid allocation size");
    } catch (const nlohmann::json::exception& error) {
        storage_error(StorageErrorCode::invalid_snapshot,
                      std::string("document JSON cannot be preserved: ") + error.what());
    } catch (const std::filesystem::filesystem_error& error) {
        storage_error(StorageErrorCode::io_error,
                      std::string("project save filesystem error: ") + error.what());
    }
}
} // namespace

SaveReceipt ProjectStore::save(const std::filesystem::path& destination,
                               const DocumentSnapshot& snapshot, const SaveOptions& options) {
    return save_project(destination, snapshot, options);
}

SaveReceipt ProjectStore::save_archive(const std::filesystem::path& destination,
                                      const ProjectArchiveSnapshot& archive, const SaveOptions& options) {
    return save_project(destination, archive.document(), options, &archive);
}

ArchiveLoadResult ProjectStore::load_archive(const std::filesystem::path& source, ArchiveRole role) {
    try {
        validate_project_path(source, true);
        for (const auto* suffix : {"-journal", "-wal", "-shm"}) {
            std::error_code error;
            if (std::filesystem::exists(sqlite_sidecar(source, suffix), error) || error)
                storage_error(StorageErrorCode::integrity_failure,
                              "standalone archive has an unexpected SQLite sidecar file");
        }
#ifdef _WIN32
        const LockedReadFile lock(source);
        auto decoded = decode_project_under_lock(source, lock.get(), role);
        if (decoded.ledger.empty())
            storage_error(StorageErrorCode::unsupported_format, "archive load requires a recovery ledger in format v4 or v5");
        if (decoded.recovery.opaque())
            return {std::nullopt, std::move(decoded.recovery), std::move(decoded.file_sha256)};
        return {ProjectArchiveSnapshot(std::move(decoded.snapshot), std::move(decoded.ledger), role),
                std::move(decoded.recovery), std::move(decoded.file_sha256)};
#else
        storage_error(StorageErrorCode::io_error, "archive load requires Windows locked-file semantics");
#endif
    } catch (const StorageError&) {
        throw;
    } catch (const DocumentError& error) {
        storage_error(StorageErrorCode::integrity_failure,
                      std::string("archive document validation failed: ") + error.what());
    } catch (const std::bad_alloc&) {
        storage_error(StorageErrorCode::resource_limit, "insufficient memory to decode recovery archive");
    } catch (const std::length_error&) {
        storage_error(StorageErrorCode::resource_limit, "archive requested an invalid allocation size");
    } catch (const nlohmann::json::exception& error) {
        storage_error(StorageErrorCode::integrity_failure, error.what());
    } catch (const std::invalid_argument& error) {
        storage_error(StorageErrorCode::integrity_failure, error.what());
    } catch (const std::filesystem::filesystem_error& error) {
        storage_error(StorageErrorCode::io_error, error.what());
    }
}

}  // namespace sketch
