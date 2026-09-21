#include "sketch/project_exchange.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/boundary_edit.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <winternl.h>

#pragma comment(lib, "bcrypt.lib")

namespace sketch {
namespace {
using Json = nlohmann::json;

std::string generic_utf8(const std::filesystem::path &path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}

void ordinary_directory(const std::filesystem::path &path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error(
        "Extraction requires an ordinary parent directory");
}

class OwnedHandle {
public:
  explicit OwnedHandle(HANDLE value) : value_(value) {
    if (value == INVALID_HANDLE_VALUE || !value)
      throw std::runtime_error(
          "Could not retain extraction object, Windows error " +
          std::to_string(GetLastError()));
    if (!GetFileInformationByHandleEx(value_, FileIdInfo, &identity_,
                                      sizeof(identity_))) {
      const auto error = GetLastError();
      CloseHandle(value_);
      value_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error(
          "Could not identify extraction object, Windows error " +
          std::to_string(error));
    }
  }
  ~OwnedHandle() { close(); }
  OwnedHandle(const OwnedHandle &) = delete;
  OwnedHandle &operator=(const OwnedHandle &) = delete;
  HANDLE get() const { return value_; }
  [[nodiscard]] bool open() const noexcept {
    return value_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] const FILE_ID_INFO &identity() const noexcept {
    return identity_;
  }
  void close() noexcept {
    if (value_ != INVALID_HANDLE_VALUE) {
      CloseHandle(value_);
      value_ = INVALID_HANDLE_VALUE;
    }
  }
  void remove() {
    if (removed_)
      return;
    if (!open())
      throw std::runtime_error(
          "Could not remove extraction object without its verified handle");
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(value_, FileDispositionInfo, &disposition,
                                    sizeof(disposition)))
      throw std::runtime_error(
          "Could not remove owned extraction object, Windows error " +
          std::to_string(GetLastError()));
    close();
    removed_ = true;
  }

private:
  HANDLE value_;
  FILE_ID_INFO identity_{};
  bool removed_{};
};

bool same_identity(const FILE_ID_INFO &left,
                   const FILE_ID_INFO &right) noexcept {
  return left.VolumeSerialNumber == right.VolumeSerialNumber &&
         !std::memcmp(&left.FileId, &right.FileId, sizeof(left.FileId));
}

std::unique_ptr<OwnedHandle> relative_object(HANDLE parent,
                                             const std::wstring &name,
                                             bool directory,
                                             ULONG disposition) {
  const auto create = std::bit_cast<decltype(&NtCreateFile)>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  if (!create || name.empty() ||
      name.find_first_of(L"\\/:") != std::wstring::npos ||
      name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
    throw std::runtime_error(
        "Unsupported extraction object name or Windows runtime");
  UNICODE_STRING relative{};
  relative.Buffer = const_cast<PWSTR>(name.data());
  relative.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  relative.MaximumLength = relative.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;
  attributes.ObjectName = &relative;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  IO_STATUS_BLOCK status{};
  HANDLE handle{};
  const ACCESS_MASK access =
      DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES |
      (directory ? FILE_LIST_DIRECTORY | FILE_TRAVERSE
                 : GENERIC_READ | GENERIC_WRITE);
  const auto result = create(
      &handle, access, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL,
      directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
      disposition,
      FILE_SYNCHRONOUS_IO_NONALERT |
          (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
          FILE_OPEN_REPARSE_POINT,
      nullptr, 0);
  if (result < 0)
    throw std::runtime_error(
        std::string(disposition == FILE_CREATE ? "Could not reserve"
                                                : "Could not reopen") +
        " extraction object, NT status " +
        std::to_string(static_cast<unsigned long>(result)));
  return std::make_unique<OwnedHandle>(handle);
}

std::unique_ptr<OwnedHandle> create_child(HANDLE parent,
                                          const std::wstring &name,
                                          bool directory) {
  // Create and retain one object in a single operation. Opening by path after
  // CreateDirectory would leave an identity substitution interval.
  return relative_object(parent, name, directory, FILE_CREATE);
}

std::unique_ptr<OwnedHandle> open_child(HANDLE parent,
                                        const std::wstring &name,
                                        bool directory) {
  return relative_object(parent, name, directory, FILE_OPEN);
}

void verify_type(HANDLE handle, bool directory) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                    sizeof(attributes)))
    throw std::runtime_error("Could not inspect extraction object type");
  const bool observed_directory =
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (observed_directory != directory ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error(
        "Extraction object type changed or contains a reparse point");
}

std::uint64_t file_size(HANDLE handle) {
  FILE_STANDARD_INFO information{};
  if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &information,
                                    sizeof(information)) ||
      information.Directory || information.EndOfFile.QuadPart < 0)
    throw std::runtime_error("Could not inspect extraction payload size");
  return static_cast<std::uint64_t>(information.EndOfFile.QuadPart);
}

std::string hash_file(HANDLE handle, std::uint64_t expected_size) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size{};
  DWORD hash_size{};
  DWORD received{};
  std::vector<unsigned char> object;
  std::vector<unsigned char> digest;
  const auto bcrypt = [](NTSTATUS status, std::string_view operation) {
    if (status < 0)
      throw std::runtime_error("BCrypt SHA-256 " + std::string(operation) +
                               " failed");
  };
  try {
    bcrypt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                       nullptr, 0),
           "initialization");
    bcrypt(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&object_size),
                             sizeof(object_size), &received, 0),
           "object-size query");
    bcrypt(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                             reinterpret_cast<PUCHAR>(&hash_size),
                             sizeof(hash_size), &received, 0),
           "digest-size query");
    object.resize(object_size);
    digest.resize(hash_size);
    bcrypt(BCryptCreateHash(algorithm, &hash, object.data(), object_size,
                            nullptr, 0, 0),
           "hash creation");
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN))
      throw std::runtime_error("Could not rewind extraction payload");
    std::vector<unsigned char> buffer(1024 * 1024);
    std::uint64_t remaining = expected_size;
    while (remaining) {
      const auto requested = static_cast<DWORD>((std::min)(
          remaining, static_cast<std::uint64_t>(buffer.size())));
      DWORD read{};
      if (!ReadFile(handle, buffer.data(), requested, &read, nullptr) ||
          read == 0)
        throw std::runtime_error("Could not read extraction payload");
      bcrypt(BCryptHashData(hash, buffer.data(), read, 0), "update");
      remaining -= read;
    }
    unsigned char extra{};
    DWORD read{};
    if (!ReadFile(handle, &extra, 1, &read, nullptr) || read != 0)
      throw std::runtime_error("Extraction payload size changed while read");
    bcrypt(BCryptFinishHash(hash, digest.data(), hash_size, 0),
           "finalization");
  } catch (...) {
    if (hash)
      BCryptDestroyHash(hash);
    if (algorithm)
      BCryptCloseAlgorithmProvider(algorithm, 0);
    throw;
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  static constexpr char hexadecimal[] = "0123456789abcdef";
  std::string result(digest.size() * 2, '0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    result[index * 2] = hexadecimal[digest[index] >> 4U];
    result[index * 2 + 1] = hexadecimal[digest[index] & 0x0fU];
  }
  return result;
}

void rename_relative(HANDLE object, HANDLE parent,
                     const std::wstring &leaf) {
  if (leaf.empty() || leaf.find_first_of(L"\\/:") != std::wstring::npos)
    throw std::runtime_error("Unsupported extraction destination name");
  const auto bytes = leaf.size() * sizeof(wchar_t);
  if (bytes > std::numeric_limits<DWORD>::max() -
                  offsetof(FILE_RENAME_INFO, FileName))
    throw std::runtime_error("Extraction destination name is too long");
  std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes, 0);
  auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
  rename->Flags = 0;
  rename->RootDirectory = parent;
  rename->FileNameLength = static_cast<DWORD>(bytes);
  std::memcpy(rename->FileName, leaf.data(), bytes);
  using NtSetInformationFilePointer = NTSTATUS(NTAPI *)(
      HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
  const auto set_information =
      std::bit_cast<NtSetInformationFilePointer>(GetProcAddress(
          GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
  if (!set_information)
    throw std::runtime_error("Windows relative rename is unavailable");
  IO_STATUS_BLOCK status{};
  // FileRenameInformationEx is native FILE_INFORMATION_CLASS value 65. Zero
  // flags forbid replacement. RootDirectory binds the leaf to the retained
  // destination parent rather than reparsing an absolute path.
  const auto result = set_information(
      object, &status, rename, static_cast<ULONG>(storage.size()),
      static_cast<FILE_INFORMATION_CLASS>(65));
  if (result < 0)
    throw std::runtime_error(
        "Could not rename extraction root, NT status " +
        std::to_string(static_cast<unsigned long>(result)));
}

void inject(const ExtractOptions &options, ExtractFaultStage stage,
            const std::filesystem::path &path) {
  if (options.stage_observer)
    options.stage_observer(stage, path);
  if (options.fault_stage == stage)
    throw std::runtime_error("Injected extraction write failure");
}

class StagingDirectory {
  struct Payload {
    std::filesystem::path relative;
    std::uint64_t size{};
    std::string hash;
    FILE_ID_INFO identity{};
    std::unique_ptr<OwnedHandle> handle;
  };

public:
  explicit StagingDirectory(const std::filesystem::path &destination) {
    parent_ = std::filesystem::canonical(destination.parent_path());
    ordinary_directory(parent_);
    path_ = parent_ / (destination.filename().wstring() + L".extract-" +
                       std::filesystem::path(make_stable_id()).wstring());
    parent_handle_ = std::make_unique<OwnedHandle>(CreateFileW(
        parent_.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                             FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    directory_ =
        create_child(parent_handle_->get(), path_.filename().wstring(), true);
    verify_type(directory_->get(), true);
  }
  void create_assets() {
    assets_ = create_child(directory_->get(), L"assets", true);
    verify_type(assets_->get(), true);
  }
  void write(const std::filesystem::path &relative, const void *bytes,
             std::size_t size) {
    const bool asset = relative.parent_path() == L"assets";
    const auto expected_hash = sha256_hex(std::span<const std::byte>(
        static_cast<const std::byte *>(bytes), size));
    auto handle = create_child(asset ? assets_->get() : directory_->get(),
                               relative.filename().wstring(), false);
    auto *output = handle.get();
    files_.push_back({relative, static_cast<std::uint64_t>(size), expected_hash,
                      output->identity(), std::move(handle)});
    const auto *cursor = static_cast<const char *>(bytes);
    while (size) {
      const auto count =
          static_cast<DWORD>((std::min)(size, std::size_t{1024 * 1024}));
      DWORD written{};
      if (!WriteFile(output->get(), cursor, count, &written, nullptr) ||
          written != count)
        throw std::runtime_error("Could not write extraction payload");
      cursor += written;
      size -= written;
    }
    if (!FlushFileBuffers(output->get()))
      throw std::runtime_error("Could not flush extraction payload");
  }
  void publish(const std::filesystem::path &destination,
               const ExtractOptions &options) {
    validate_retained(path_);
    // Windows refuses directory rename with open descendants, including
    // deletion-shared descendants. Release their guards only at this last
    // step. The root object remains locked. This final interval is not a
    // security boundary against same-user descendant modifications.
    for (auto &file : files_)
      file.handle->close();
    assets_->close();
    rename_relative(directory_->get(), parent_handle_->get(),
                    destination.filename().wstring());
    try {
      inject(options, ExtractFaultStage::after_publish, destination);
      validate_reopened(destination);
    } catch (...) {
      const auto original = std::current_exception();
      try {
        rename_relative(directory_->get(), parent_handle_->get(),
                        path_.filename().wstring());
      } catch (const std::exception &rollback_error) {
        active_ = false;
        std::string original_message = "Unknown post-publication failure";
        try {
          std::rethrow_exception(original);
        } catch (const std::exception &error) {
          original_message = error.what();
        } catch (...) {
        }
        throw ExtractionCleanupError(
            original_message + "; post-publication rollback failed: " +
                rollback_error.what() + "; residual destination: " +
                generic_utf8(destination),
            destination);
      }
      std::rethrow_exception(original);
    }
    active_ = false;
  }
  void cleanup() {
    if (!active_)
      return;
    // Delete only the exact objects created by this operation. A foreign
    // child prevents directory removal and is reported, never traversed.
    std::string failures;
    const auto failure = [&](const std::exception &error) {
      failures += std::string(error.what()) + "; ";
    };
    try {
      reopen_for_cleanup();
    } catch (const std::exception &error) {
      failure(error);
    }
    const auto remove = [&](OwnedHandle *handle) {
      if (!handle)
        return;
      try {
        handle->remove();
      } catch (const std::exception &error) {
        failure(error);
      }
    };
    for (auto it = files_.rbegin(); it != files_.rend(); ++it)
      remove(it->handle.get());
    remove(assets_.get());
    if (failures.empty())
      remove(directory_.get());
    if (!failures.empty())
      throw std::runtime_error(failures);
    active_ = false;
  }
  ~StagingDirectory() {
    try {
      cleanup();
    } catch (...) { /* Explicit failure handling reports residual data. */
    }
  }
  const std::filesystem::path &path() const { return path_; }
  void preserve_residual() noexcept { active_ = false; }

private:
  void verify_payload(HANDLE handle, const struct Payload &payload) const {
    verify_type(handle, false);
    if (file_size(handle) != payload.size ||
        hash_file(handle, payload.size) != payload.hash)
      throw std::runtime_error(
          "Extraction payload size or SHA-256 changed");
  }

  void verify_tree(const std::filesystem::path &root) const {
    std::set<std::filesystem::path> root_expected{L"assets"};
    std::set<std::filesystem::path> asset_expected;
    for (const auto &file : files_) {
      if (file.relative.parent_path() == L"assets") {
        asset_expected.insert(file.relative.filename());
      } else if (file.relative.parent_path().empty()) {
        root_expected.insert(file.relative.filename());
      } else {
        throw std::runtime_error("Unsupported extraction payload path");
      }
    }
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      if (!root_expected.erase(entry.path().filename()))
        throw std::runtime_error(
            "Unexpected content in extraction directory");
    }
    if (!root_expected.empty())
      throw std::runtime_error("Missing extraction content");
    for (const auto &entry :
         std::filesystem::directory_iterator(root / L"assets")) {
      if (!asset_expected.erase(entry.path().filename()))
        throw std::runtime_error(
            "Unexpected content in extraction assets directory");
    }
    if (!asset_expected.empty())
      throw std::runtime_error("Missing extraction asset content");
  }

  void validate_retained(const std::filesystem::path &root) const {
    verify_type(directory_->get(), true);
    verify_type(assets_->get(), true);
    for (const auto &file : files_)
      verify_payload(file.handle->get(), file);
    verify_tree(root);
  }

  void validate_reopened(const std::filesystem::path &destination) const {
    verify_type(directory_->get(), true);
    auto assets = open_child(directory_->get(), L"assets", true);
    if (!same_identity(assets->identity(), assets_->identity()))
      throw std::runtime_error(
          "Post-publication assets identity changed");
    verify_type(assets->get(), true);
    std::vector<std::unique_ptr<OwnedHandle>> guards;
    guards.reserve(files_.size());
    for (const auto &file : files_) {
      auto guard = open_child(file.relative.parent_path() == L"assets"
                                  ? assets->get()
                                  : directory_->get(),
                              file.relative.filename().wstring(), false);
      if (!same_identity(guard->identity(), file.identity))
        throw std::runtime_error(
            "Post-publication payload identity changed");
      verify_payload(guard->get(), file);
      guards.push_back(std::move(guard));
    }
    verify_tree(destination);
  }

  void reopen_for_cleanup() {
    if (assets_ && !assets_->open()) {
      auto reopened = open_child(directory_->get(), L"assets", true);
      if (!same_identity(reopened->identity(), assets_->identity()))
        throw std::runtime_error(
            "Extraction assets identity changed; preserving foreign content");
      verify_type(reopened->get(), true);
      assets_ = std::move(reopened);
    }
    for (auto &file : files_) {
      if (file.handle->open())
        continue;
      auto reopened = open_child(file.relative.parent_path() == L"assets"
                                     ? assets_->get()
                                     : directory_->get(),
                                 file.relative.filename().wstring(), false);
      if (!same_identity(reopened->identity(), file.identity))
        throw std::runtime_error(
            "Extraction payload identity changed; preserving foreign content");
      verify_type(reopened->get(), false);
      file.handle = std::move(reopened);
    }
  }

  std::filesystem::path parent_, path_;
  std::unique_ptr<OwnedHandle> parent_handle_, directory_, assets_;
  std::vector<Payload> files_;
  bool active_{true};
};
Json entity_json(const Entity &entity) {
  return {{"id", entity.id},
          {"type", entity.type},
          {"required", entity.required},
          {"properties", entity.properties},
          {"extensions", entity.extensions}};
}
} // namespace

void extract_project(const DocumentSnapshot &snapshot,
                     const std::filesystem::path &requested,
                     const ExtractOptions &options) {
  auto destination = std::filesystem::absolute(requested).lexically_normal();
  if (requested.empty() || destination.filename().empty() ||
      destination.filename() == "." || destination.filename() == ".." ||
      std::filesystem::exists(destination))
    throw std::runtime_error(
        "Extraction destination must name a new directory");
  ordinary_directory(destination.parent_path());
  destination = std::filesystem::canonical(destination.parent_path()) /
                destination.filename();
  StagingDirectory staging(destination);
  try {
    const auto &temporary = staging.path();
    staging.create_assets();
    Json document = {{"document_id", snapshot.document_id()},
                     {"revision", snapshot.revision()},
                     {"named_revisions", snapshot.named_revisions()},
                     {"editable", snapshot.is_editable()},
                     {"read_only_reason", snapshot.read_only_reason()}};
    document["saved_revision"] = snapshot.saved_revision_optional()
                                     ? Json(*snapshot.saved_revision_optional())
                                     : Json(nullptr);
    Json result = {{"exchange_format", "vertex-json-assets"},
                   {"exchange_version", 1},
                   {"document", std::move(document)},
                   {"revisions", Json::array()}};
    std::set<std::string> written_assets;
    for (const auto &revision : snapshot.history()) {
      Json row = {{"revision", revision.revision},
                  {"action", revision.action},
                  {"undo_stack", revision.undo_stack},
                  {"redo_stack", revision.redo_stack},
                  {"entities", Json::array()},
                  {"assets", Json::array()}};
      row["parent_revision"] = revision.parent_revision
                                   ? Json(*revision.parent_revision)
                                   : Json(nullptr);
      row["source_revision"] = revision.source_revision
                                   ? Json(*revision.source_revision)
                                   : Json(nullptr);
      row["name"] = revision.name ? Json(*revision.name) : Json(nullptr);
      if (revision.boundary_translation) {
        if (result["exchange_version"] == 1) result["exchange_version"] = 2;
        row["boundary_translation"] = encode_boundary_translation(*revision.boundary_translation);
      }
      if (revision.boundary_transform) {
        if (result["exchange_version"].get<int>() < 3) result["exchange_version"] = 3;
        row["boundary_transform"] = encode_boundary_transform(*revision.boundary_transform);
      }
      if (revision.boundary_geometry_edit) {
        result["exchange_version"] = 4;
        row["boundary_geometry_edit"] =
            encode_boundary_geometry_edit(*revision.boundary_geometry_edit);
      }
      for (const auto &[id, entity] : revision.entities) {
        (void)id;
        row["entities"].push_back(entity_json(entity));
      }
      for (const auto &[id, asset] : revision.assets) {
        const auto hash = sha256_hex(asset.bytes);
        const auto relative = std::filesystem::path("assets") / (hash + ".bin");
        if (written_assets.insert(hash).second) {
          staging.write(relative, asset.bytes.data(), asset.bytes.size());
        }
        row["assets"].push_back({{"id", id},
                                 {"media_type", asset.media_type},
                                 {"sha256", hash},
                                 {"metadata", asset.metadata},
                                 {"path", generic_utf8(relative)}});
      }
      result["revisions"].push_back(std::move(row));
    }
    inject(options, ExtractFaultStage::after_assets, temporary);
    const auto metadata = result.dump(2) + '\n';
    staging.write(L"project.json", metadata.data(), metadata.size());
    inject(options, ExtractFaultStage::before_publish, temporary);
    staging.publish(destination, options);
  } catch (...) {
    const auto original = std::current_exception();
    try {
      staging.cleanup();
    } catch (const std::exception &cleanup_error) {
      staging.preserve_residual();
      std::string original_message = "Unknown extraction failure";
      try {
        std::rethrow_exception(original);
      } catch (const std::exception &error) {
        original_message = error.what();
      } catch (...) {
      }
      throw ExtractionCleanupError(
          original_message + "; cleanup failed: " + cleanup_error.what() +
              "; residual staging: " + generic_utf8(staging.path()),
          staging.path());
    }
    std::rethrow_exception(original);
  }
}
} // namespace sketch
