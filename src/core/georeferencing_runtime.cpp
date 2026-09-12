#include "sketch/georeferencing_runtime.hpp"

#include "sketch/document.hpp"

#include <proj.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

using Path = std::filesystem::path;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument(message);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z'
                                     ? character - ('A' - 'a')
                                     : character);
    });
    return value;
}

std::string path_utf8(const Path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

Path canonical_directory(const Path& value) {
    require(!value.empty(), "PROJ resource root is required");
    std::error_code error;
    const auto resolved = std::filesystem::canonical(value, error);
    require(!error && std::filesystem::is_directory(resolved, error) && !error,
            "PROJ resource root must be an existing directory");
    return resolved;
}

Path contained_file(const Path& root, const std::string& relative_path) {
    const auto candidate = root / Path(relative_path);
    std::error_code error;
    const auto resolved = std::filesystem::canonical(candidate, error);
    require(!error && std::filesystem::is_regular_file(resolved, error) && !error,
            "declared PROJ resource is not a regular file: " + relative_path);
    const auto relative = resolved.lexically_relative(root);
    require(!relative.empty() && !relative.is_absolute(),
            "declared PROJ resource escapes its root: " + relative_path);
    for (const auto& part : relative) {
        require(part != Path(".."),
                "declared PROJ resource escapes its root: " + relative_path);
    }
    return resolved;
}

std::vector<std::byte> read_file(const Path& path, std::uint64_t maximum_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error, "could not determine PROJ resource size");
    require(size <= maximum_bytes, "PROJ resource exceeds the configured size limit");
    require(size <= static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()),
            "PROJ resource is too large for this process");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.is_open(), "could not open PROJ resource");
    const auto position = input.tellg();
    require(position >= 0, "could not seek PROJ resource");
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        require(input.gcount() == static_cast<std::streamsize>(bytes.size()),
                "could not read PROJ resource");
    }
    return bytes;
}

std::string proj_error(PJ_CONTEXT* context, std::string_view operation) {
    const auto code = proj_context_errno(context);
    const auto* detail = code == 0 ? nullptr : proj_context_errno_string(context, code);
    std::ostringstream message;
    message << operation << " (PROJ error " << code;
    if (detail != nullptr) message << ": " << detail;
    message << ')';
    return message.str();
}

struct FileFinderData {
    std::map<std::string, std::string> files_by_name;
};

const char* find_declared_file(PJ_CONTEXT*, const char* name, void* user_data) {
    if (name == nullptr || user_data == nullptr) return nullptr;
    auto* data = static_cast<FileFinderData*>(user_data);
    const auto key = lower(std::filesystem::path(name).filename().string());
    const auto found = data->files_by_name.find(key);
    return found == data->files_by_name.end() ? nullptr : found->second.c_str();
}

struct ContextDeleter {
    void operator()(PJ_CONTEXT* context) const noexcept {
        if (context != nullptr) (void)proj_context_destroy(context);
    }
};

struct PjDeleter {
    void operator()(PJ* object) const noexcept {
        if (object != nullptr) (void)proj_destroy(object);
    }
};

using Context = std::unique_ptr<PJ_CONTEXT, ContextDeleter>;
using Pj = std::unique_ptr<PJ, PjDeleter>;

// PROJ 9.8's legacy debug Windows DLL reads PROJ_DATA while creating its
// context even when the context search-path API has been supplied. Pin that
// variable to the verified local directory for the short preflight and restore
// the caller's value before returning. The mutex keeps concurrent preflights
// from racing over this process-global compatibility variable.
class ScopedProjData {
public:
    explicit ScopedProjData(std::string value) : lock_(mutex()) {
#ifdef _WIN32
        char* previous = nullptr;
        std::size_t previous_size = 0;
        if (_dupenv_s(&previous, &previous_size, "PROJ_DATA") == 0 && previous != nullptr) {
            had_previous_ = true;
            previous_ = previous;
        }
        std::free(previous);
        const auto result = _putenv_s("PROJ_DATA", value.c_str());
#else
        const auto* previous = std::getenv("PROJ_DATA");
        if (previous != nullptr) {
            had_previous_ = true;
            previous_ = previous;
        }
        const auto result = setenv("PROJ_DATA", value.c_str(), 1);
#endif
        require(result == 0, "could not pin PROJ_DATA to the local resource tree");
    }

    ScopedProjData(const ScopedProjData&) = delete;
    ScopedProjData& operator=(const ScopedProjData&) = delete;

    ~ScopedProjData() {
#ifdef _WIN32
        (void)_putenv_s("PROJ_DATA", had_previous_ ? previous_.c_str() : "");
#else
        if (had_previous_) (void)setenv("PROJ_DATA", previous_.c_str(), 1);
        else (void)unsetenv("PROJ_DATA");
#endif
    }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }

    std::unique_lock<std::mutex> lock_;
    bool had_previous_{};
    std::string previous_;
};

void validate_object(PJ_CONTEXT* context, const char* definition,
                     std::string_view label) {
    Pj object(proj_create(context, definition));
    require(object != nullptr, std::string(label) + " could not be parsed: " +
                                  proj_error(context, label));
    require(proj_errno(object.get()) == 0,
            std::string(label) + " reported an error: " + proj_error(context, label));
}

}  // namespace

nlohmann::json GeoreferencingRuntimeReport::to_json() const {
    auto resources_json = nlohmann::json::array();
    for (const auto& resource : resources) {
        resources_json.push_back({{"relative_path", resource.relative_path},
                                  {"sha256", resource.sha256},
                                  {"size_bytes", resource.size_bytes}});
    }
    return { {"version", 1},
             {"runtime", "PROJ"},
             {"proj_version", proj_version},
             {"crs_identifier", crs_identifier},
             {"database_relative_path", database_relative_path},
             {"network_enabled", network_enabled},
             {"crs_identifier_validated", crs_identifier_validated},
             {"crs_definition_validated", crs_definition_validated},
             {"identity_transform_validated", identity_transform_validated},
             {"resources", std::move(resources_json)} };
}

GeoreferencingRuntimeReport verify_georeferencing_runtime(
    const GeoreferencingContract& contract,
    const GeoreferencingRuntimeOptions& options) {
    require(options.maximum_resource_bytes > 0 &&
                options.maximum_resource_bytes <= 1024ULL * 1024 * 1024,
            "invalid PROJ resource size limit");
    const auto root = canonical_directory(options.resource_root);

    GeoreferencingRuntimeReport report;
    report.crs_identifier = contract.crs().identifier;
    std::vector<std::string> search_paths;
    std::set<std::string> search_path_keys;
    FileFinderData file_finder;
    std::optional<Path> database_path;
    for (const auto& declared : contract.offline_resources().files) {
        const auto resolved = contained_file(root, declared.relative_path);
        const auto bytes = read_file(resolved, options.maximum_resource_bytes);
        const auto digest = sha256_hex(bytes);
        require(digest == declared.sha256,
                "PROJ resource SHA-256 mismatch: " + declared.relative_path);

        report.resources.push_back({declared.relative_path, digest,
                                   static_cast<std::uint64_t>(bytes.size())});
        const auto file_name = lower(resolved.filename().string());
        const auto [file_it, inserted] = file_finder.files_by_name.emplace(
            file_name, path_utf8(resolved));
        require(inserted,
                "multiple PROJ resources share the same filename: " + file_name);
        (void)file_it;
        const auto parent = resolved.parent_path();
        const auto parent_text = path_utf8(parent);
        if (search_path_keys.insert(lower(parent_text)).second) {
            search_paths.push_back(parent_text);
        }
        if (lower(resolved.filename().string()) == "proj.db") {
            require(!database_path.has_value(),
                    "multiple PROJ databases were declared");
            database_path = resolved;
            report.database_relative_path = declared.relative_path;
        }
    }
    require(database_path.has_value(),
            "a declared proj.db resource is required for PROJ runtime");

    ScopedProjData proj_data_guard(path_utf8(database_path->parent_path()));
    (void)proj_data_guard;
    const auto context_raw = proj_context_create();
    require(context_raw != nullptr, "could not create a PROJ context");
    Context context(context_raw);
    // PROJ returns whether network access is available from the configured
    // backend, not whether the requested flag was accepted. A disabled
    // context therefore legitimately returns zero on builds without a
    // network backend; inspect the context state below for the contract.
    (void)proj_context_set_enable_network(context.get(), 0);
    report.network_enabled = proj_context_is_network_enabled(context.get()) != 0;
    require(!report.network_enabled, "PROJ networking could not be disabled");
    proj_context_set_file_finder(context.get(), find_declared_file, &file_finder);
    const auto root_text = path_utf8(root);
    if (search_path_keys.insert(lower(root_text)).second) search_paths.push_back(root_text);
    require(search_paths.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
            "too many PROJ search paths");
    std::vector<const char*> search_path_pointers;
    search_path_pointers.reserve(search_paths.size());
    for (const auto& path : search_paths) search_path_pointers.push_back(path.c_str());
    const auto database_text = path_utf8(*database_path);
    require(proj_context_set_database_path(context.get(), database_text.c_str(), nullptr,
                                           nullptr) != 0,
            "could not bind PROJ database: " + proj_error(context.get(), "database"));
    // Setting the database can reset the legacy search-path list in some PROJ
    // builds. Apply it last so debug and release DLLs use the same contained
    // resource tree.
    proj_context_set_search_paths(context.get(),
                                  static_cast<int>(search_path_pointers.size()),
                                  search_path_pointers.data());

    validate_object(context.get(), contract.crs().identifier.c_str(), "CRS identifier");
    report.crs_identifier_validated = true;
    validate_object(context.get(), contract.crs().definition.c_str(), "CRS definition");
    report.crs_definition_validated = true;

    Pj operation(proj_create_crs_to_crs(context.get(), contract.crs().identifier.c_str(),
                                         contract.crs().identifier.c_str(), nullptr));
    require(operation != nullptr,
            "could not construct the PROJ identity operation: " +
                proj_error(context.get(), "identity operation"));
    Pj normalized(proj_normalize_for_visualization(context.get(), operation.get()));
    require(normalized != nullptr,
            "could not normalize the PROJ identity operation: " +
                proj_error(context.get(), "identity operation"));
    const auto probe = proj_trans(normalized.get(), PJ_FWD, proj_coord(0, 0, 0, 0));
    require(std::isfinite(probe.xy.x) && std::isfinite(probe.xy.y) &&
                proj_errno(normalized.get()) == 0,
            "PROJ identity operation could not transform a finite coordinate: " +
                proj_error(context.get(), "identity operation"));
    report.identity_transform_validated = true;
    const auto info = proj_info();
    report.proj_version = info.version == nullptr ? std::string{} : info.version;
    require(!report.proj_version.empty(), "PROJ did not report a version");
    return report;
}

}  // namespace sketch
