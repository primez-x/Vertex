#include "sketch/project_resource_catalog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace sketch {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
    throw std::invalid_argument(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

bool is_reparse_or_symlink(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error))) {
        return true;
    }
    if (error) fail("could not inspect package path: " + path.string());
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
#endif
    return false;
}

void reject_link_chain(const std::filesystem::path& path, const std::string& field) {
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        if (is_reparse_or_symlink(current)) {
            fail(field + " contains a symlink or reparse point: " + path.string());
        }
        if (current == current.root_path()) break;
    }
}

std::filesystem::path resolve_package(const std::filesystem::path& input) {
    require(!input.empty(), "package root is required");
    reject_link_chain(input, "package root");
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(input, error);
    if (error || !std::filesystem::is_directory(root)) {
        fail("package root is not a directory: " + input.string());
    }
    reject_link_chain(root, "package root");
    return root;
}

std::filesystem::path safe_relative_path(const Json& value, const std::string& field) {
    require(value.is_string(), field + " must be a string");
    auto text = value.get<std::string>();
    require(!text.empty() && text.find('\0') == std::string::npos,
            field + " must be a nonempty path");
    require(text.front() != '/' && text.front() != '\\' &&
                !(text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
                  text[1] == ':'),
            field + " must be relative");
    std::replace(text.begin(), text.end(), '\\', '/');
    std::size_t segment_start = 0;
    while (segment_start <= text.size()) {
        const auto segment_end = text.find('/', segment_start);
        const auto segment = text.substr(
            segment_start, segment_end == std::string::npos
                              ? std::string::npos : segment_end - segment_start);
        require(!segment.empty() && segment != "." && segment != "..",
                field + " contains a noncanonical path component");
        require(segment.find(':') == std::string::npos,
                field + " cannot contain an alternate data stream");
        if (segment_end == std::string::npos) break;
        segment_start = segment_end + 1;
    }
    const auto path = std::filesystem::path(text).lexically_normal();
    require(!path.empty() && !path.has_root_path() && path != ".",
            field + " must be a normalized relative path");
    for (const auto& part : path) {
        require(part != ".." && part != "." && part != "",
                field + " contains an unsafe path component");
    }
    return path;
}

std::string sha256_file(const std::filesystem::path& path, std::uint64_t& size) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open resource payload: " + path.string());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    const auto check_status = [](NTSTATUS status, const char* operation) {
        if (status < 0) fail(std::string("SHA-256 ") + operation + " failed");
    };
    try {
        check_status(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                 nullptr, 0), "initialization");
        check_status(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                       reinterpret_cast<PUCHAR>(&object_size),
                                       sizeof(object_size), &received, 0),
                     "object-size query");
        check_status(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                       reinterpret_cast<PUCHAR>(&hash_size),
                                       sizeof(hash_size), &received, 0),
                     "hash-size query");
        require(object_size > 0 && hash_size == 32, "unexpected SHA-256 provider sizes");
        object.resize(object_size);
        digest.resize(hash_size);
        check_status(BCryptCreateHash(algorithm, &hash, object.data(), object_size,
                                      nullptr, 0, 0), "creation");

        // Keep the streaming buffer on the heap; a megabyte automatic array
        // would exhaust the small Windows test-thread stack.
        std::vector<char> buffer(1024 * 1024);
        size = 0;
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto received_bytes = input.gcount();
            if (received_bytes <= 0) break;
            check_status(BCryptHashData(hash,
                                        reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(received_bytes), 0),
                         "update");
            size += static_cast<std::uint64_t>(received_bytes);
        }
        require(input.eof(), "could not read resource payload: " + path.string());
        check_status(BCryptFinishHash(hash, digest.data(), hash_size, 0), "finalization");
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
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

std::string sha256_text(const Json& value, const std::string& field) {
    require(value.is_string(), field + " must be a string");
    const auto text = value.get<std::string>();
    require(text.size() == 64 &&
                std::all_of(text.begin(), text.end(), [](unsigned char c) {
                    return std::isxdigit(c) != 0;
                }),
            field + " must be a SHA-256 digest");
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::uint64_t size_value(const Json& value, const std::string& field) {
    require(value.is_number_unsigned() || value.is_number_integer(),
            field + " must be a nonnegative integer");
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        require(signed_value >= 0, field + " must be nonnegative");
        return static_cast<std::uint64_t>(signed_value);
    }
    return value.get<std::uint64_t>();
}

std::string required_name(const Json& value, const std::string& field) {
    require(value.is_string(), field + " must be a string");
    auto result = value.get<std::string>();
    require(!result.empty() && result.size() <= 256 &&
                result.find('\0') == std::string::npos,
            field + " must be a bounded nonempty name");
    return result;
}

std::string folded(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct FileRecord {
    std::string kind;
    std::string sha256;
    std::uint64_t size{};
};

std::vector<std::uint8_t> read_payload(const std::filesystem::path& path,
                                       const ProjectResource& resource) {
    std::uint64_t size = 0;
    const auto digest = sha256_file(path, size);
    require(size == resource.size && digest == resource.sha256,
            "resource payload hash or size changed: " + resource.relative_path.generic_string());
    require(size <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()),
            "resource payload is too large to read");
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not reopen resource payload: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        require(input.gcount() == static_cast<std::streamsize>(bytes.size()),
                "resource payload changed while reading: " + resource.relative_path.generic_string());
        char extra{};
        input.read(&extra, 1);
        require(input.gcount() == 0 && input.eof(),
                "resource payload grew while reading: " + resource.relative_path.generic_string());
    }
    return bytes;
}

}  // namespace

const char* project_resource_kind_name(ProjectResourceKind kind) noexcept {
    switch (kind) {
    case ProjectResourceKind::template_resource: return "template";
    case ProjectResourceKind::profile: return "profile";
    case ProjectResourceKind::documentation: return "documentation";
    }
    return "";
}

ProjectResourceKind project_resource_kind_from_name(std::string_view name) {
    if (name == "template") return ProjectResourceKind::template_resource;
    if (name == "profile") return ProjectResourceKind::profile;
    if (name == "documentation") return ProjectResourceKind::documentation;
    fail("unsupported project resource kind: " + std::string(name));
}

ProjectResourceCatalog ProjectResourceCatalog::register_package(
    const std::filesystem::path& package_root) {
    const auto root = resolve_package(package_root);
    const auto manifest_path = root / "project-package-manifest.json";
    reject_link_chain(manifest_path, "project package manifest");
    std::ifstream input(manifest_path, std::ios::binary);
    require(input.good(), "project package manifest is missing");
    Json manifest;
    try {
        input >> manifest;
    } catch (const Json::exception& error) {
        fail(std::string("project package manifest is malformed: ") + error.what());
    }
    try {
        return [&]() -> ProjectResourceCatalog {
    require(manifest.is_object(), "project package manifest must be an object");
    require(manifest.value("schema_version", 0) == 1 &&
                manifest.value("manifest_version", 0) == 1 &&
                manifest.value("manifest_kind", "") == "project-package",
            "unsupported project package manifest");
    require(manifest.value("audit_status", "") == "incomplete" &&
                manifest.value("offline_qualified", true) == false,
            "project package cannot claim qualification");
    require(manifest.contains("files") && manifest.at("files").is_array(),
            "project package files must be an array");
    require(manifest.contains("resources") && manifest.at("resources").is_array(),
            "project package resources must be an array");

    std::map<std::string, FileRecord> files;
    for (std::size_t index = 0; index < manifest.at("files").size(); ++index) {
        const auto& row = manifest.at("files").at(index);
        require(row.is_object(), "project package file record must be an object");
        const auto relative = safe_relative_path(row.value("path", Json{}),
                                                 "project package file path");
        const auto key = folded(relative.generic_string());
        require(files.emplace(key, FileRecord{
            required_name(row.value("kind", Json{}), "project package file kind"),
            sha256_text(row.value("sha256", Json{}), "project package file hash"),
            size_value(row.value("size", Json{}), "project package file size")}).second,
                "project package repeats file path: " + relative.generic_string());
    }

    ProjectResourceCatalog catalog;
    catalog.package_root_ = root;
    std::set<std::string> resource_names;
    std::set<std::string> resource_paths;
    for (std::size_t index = 0; index < manifest.at("resources").size(); ++index) {
        const auto& row = manifest.at("resources").at(index);
        require(row.is_object(), "project package resource record must be an object");
        const auto kind_name = required_name(row.value("kind", Json{}),
                                             "project package resource kind");
        const auto kind = project_resource_kind_from_name(kind_name);
        const auto name = required_name(row.value("name", Json{}),
                                        "project package resource name");
        const auto relative = safe_relative_path(row.value("path", Json{}),
                                                 "project package resource path");
        const auto path_key = folded(relative.generic_string());
        const auto name_key = std::string(project_resource_kind_name(kind)) + "\n" + folded(name);
        require(resource_paths.insert(path_key).second,
                "project package repeats resource path: " + relative.generic_string());
        require(resource_names.insert(name_key).second,
                "project package repeats resource name: " + name);
        const auto file = files.find(path_key);
        require(file != files.end(), "project package resource is not listed as a file: " +
                                         relative.generic_string());
        require(file->second.kind == kind_name,
                "project package resource kind disagrees with its file record: " +
                    relative.generic_string());
        const auto digest = sha256_text(row.value("sha256", Json{}),
                                        "project package resource hash");
        const auto size = size_value(row.value("size", Json{}),
                                     "project package resource size");
        require(digest == file->second.sha256 && size == file->second.size,
                "project package resource metadata disagrees with its file record: " +
                    relative.generic_string());
        const auto payload = root / relative;
        reject_link_chain(payload, "project package resource");
        require(std::filesystem::is_regular_file(payload),
                "project package resource is missing: " + relative.generic_string());
        std::uint64_t observed_size = 0;
        const auto observed_digest = sha256_file(payload, observed_size);
        require(observed_size == size && observed_digest == digest,
                "project package resource hash or size mismatch: " +
                    relative.generic_string());
        if (kind != ProjectResourceKind::documentation) {
            std::ifstream structured(payload, std::ios::binary);
            try {
                const auto decoded = Json::parse(structured);
                require(decoded.is_object(),
                        "project package structured resource must contain an object: " +
                            relative.generic_string());
            } catch (const Json::exception& error) {
                fail(std::string("project package structured resource is malformed: ") +
                     error.what());
            }
        }
        catalog.resources_.push_back(
            ProjectResource{kind, name, relative, digest, size});
    }

    // Reject files that appeared beside the manifest without a corresponding
    // package record. The manifest itself is intentionally outside `files`.
    std::error_code iterator_error;
    for (std::filesystem::recursive_directory_iterator iterator(root, iterator_error), end;
         iterator != end; iterator.increment(iterator_error)) {
        require(!iterator_error, "could not enumerate project package");
        const auto path = iterator->path();
        reject_link_chain(path, "project package");
        if (iterator->is_directory()) continue;
        const auto relative = path.lexically_relative(root).generic_string();
        if (folded(relative) == "project-package-manifest.json") continue;
        require(files.contains(folded(relative)),
                "project package contains an unlisted file: " + relative);
    }

    std::sort(catalog.resources_.begin(), catalog.resources_.end(),
              [](const auto& left, const auto& right) {
                  const auto left_kind = std::string(project_resource_kind_name(left.kind));
                  const auto right_kind = std::string(project_resource_kind_name(right.kind));
                  return std::tie(left_kind, left.name, left.relative_path) <
                         std::tie(right_kind, right.name, right.relative_path);
              });
    return catalog;
        }();
    } catch (const Json::exception& error) {
        fail(std::string("project package manifest has an invalid value: ") + error.what());
    }
}

std::vector<ProjectResource> ProjectResourceCatalog::resources(ProjectResourceKind kind) const {
    std::vector<ProjectResource> result;
    for (const auto& resource : resources_) {
        if (resource.kind == kind) result.push_back(resource);
    }
    return result;
}

std::optional<ProjectResource> ProjectResourceCatalog::find(ProjectResourceKind kind,
                                                              std::string_view name) const {
    const auto found = std::find_if(resources_.begin(), resources_.end(),
                                    [&](const auto& resource) {
                                        return resource.kind == kind && resource.name == name;
                                    });
    if (found == resources_.end()) return std::nullopt;
    return *found;
}

std::vector<std::uint8_t> ProjectResourceCatalog::read(ProjectResourceKind kind,
                                                        std::string_view name) const {
    const auto resource = find(kind, name);
    require(resource.has_value(), "project resource is not registered: " + std::string(name));
    const auto path = package_root_ / resource->relative_path;
    reject_link_chain(path, "project resource");
    require(std::filesystem::is_regular_file(path),
            "registered project resource is missing: " + resource->relative_path.generic_string());
    return read_payload(path, *resource);
}

}  // namespace sketch
