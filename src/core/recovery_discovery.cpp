#include "sketch/recovery_discovery.hpp"
#include "sketch/project_store.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sketch {
namespace {
namespace fs = std::filesystem;

bool traversal(const fs::path& path) {
    return std::any_of(path.begin(), path.end(), [](const auto& part) { return part == ".."; });
}
fs::path checked_path(const fs::path& path) {
    if (path.empty() || traversal(path)) throw std::runtime_error("empty or traversing path");
    const auto absolute = fs::absolute(path).lexically_normal();
    fs::path prefix;
    for (const auto& part : absolute) {
        prefix /= part;
        // Root-name alone (C:) is not a complete Windows path.
        if (prefix == absolute.root_name()) continue;
        const auto status = fs::symlink_status(prefix);
        if (fs::is_symlink(status)) throw std::runtime_error("symbolic link path rejected");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("reparse point path rejected");
#endif
    }
    return absolute;
}
bool same_path(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const auto left = a.generic_wstring(), right = b.generic_wstring();
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
bool candidate_name(const fs::path& path) {
    const auto name = path.filename().generic_string();
    return name.starts_with("recovery-") && name.ends_with(".bldproj");
}
RecoverySourceMatch match(const RecoveryCopyRecord& record, const fs::path& source,
                          const std::string& hash) {
    if (!record.source_path || !record.source_sha256) return RecoverySourceMatch::missing_provenance;
    const auto& text = *record.source_path;
    const auto path = fs::path(std::u8string(text.begin(), text.end()));
    if (!path.is_absolute() || traversal(path)) return RecoverySourceMatch::invalid_source_path;
    if (!same_path(path.lexically_normal(), source)) return RecoverySourceMatch::different_path;
    return *record.source_sha256 == hash ? RecoverySourceMatch::matched : RecoverySourceMatch::hash_mismatch;
}
}  // namespace

RecoveryDiscoveryResult discover_recovery_copies(const std::optional<fs::path>& source_project,
                                                  const fs::path& recovery_directory) {
    RecoveryDiscoveryResult result;
    std::optional<fs::path> source;
    if (source_project) {
        try {
            source = checked_path(*source_project);
            if (!fs::is_regular_file(fs::symlink_status(*source))) throw std::runtime_error("source is not a regular file");
            result.source_sha256 = ProjectStore::file_sha256(*source);
        } catch (const std::exception& error) { result.source_diagnostic = error.what(); source.reset(); }
    }
    std::vector<fs::path> paths;
    try {
        const auto directory = checked_path(recovery_directory);
        if (!fs::exists(directory)) return result;
        if (!fs::is_directory(fs::symlink_status(directory))) throw std::runtime_error("recovery directory is not a directory");
        std::size_t entries = 0;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (++entries > 4096) throw std::runtime_error("recovery directory exceeds 4096 entry limit");
            if (!candidate_name(entry.path())) continue;
            // Nonfiles are excluded without following them, even if named like an archive.
            if (!fs::is_regular_file(entry.symlink_status())) continue;
            try { paths.push_back(checked_path(entry.path())); }
            catch (const std::exception&) { continue; }
        }
    } catch (const std::exception& error) { result.directory_diagnostic = error.what(); return result; }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        RecoveryCandidate candidate;
        candidate.path = path;
        candidate.source_match = !source_project ? RecoverySourceMatch::not_requested :
            (!source ? RecoverySourceMatch::source_unavailable : RecoverySourceMatch::missing_provenance);
        try {
            // Recheck immediately before the storage read; discovery is not a
            // retained handle or a promise about subsequent file identity.
            (void)checked_path(path);
            if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error("candidate is not a regular file");
            const auto loaded = ProjectStore::load_archive(path, ArchiveRole::recovery_copy);
            candidate.file_sha256 = loaded.file_sha256;
            if (!loaded.supported()) candidate.reason = loaded.recovery.diagnostic;
            else if (!loaded.recovery.decoded->recovery_copy) candidate.reason = "missing recovery copy provenance";
            else {
                candidate.metadata = *loaded.recovery.decoded->recovery_copy;
                candidate.loadable = true;
                if (source) candidate.source_match = match(*candidate.metadata, *source, *result.source_sha256);
            }
            if (!candidate.loadable && candidate.reason.empty()) candidate.reason = "unsupported recovery archive";
        } catch (const std::exception& error) { candidate.reason = error.what(); candidate.loadable = false; }
        result.candidates.push_back(std::move(candidate));
    }
    std::map<std::string, std::vector<std::size_t>> identities;
    for (std::size_t i = 0; i < result.candidates.size(); ++i)
        if (result.candidates[i].metadata) identities[result.candidates[i].metadata->archive_id].push_back(i);
    for (const auto& [id, indices] : identities) {
        (void)id;
        if (indices.size() < 2) continue;
        for (const auto index : indices) {
            auto& candidate = result.candidates[index];
            candidate.duplicate_archive_id = true;
            candidate.loadable = false;
            candidate.reason = "duplicate recovery archive identity";
        }
    }
    return result;
}
}  // namespace sketch
