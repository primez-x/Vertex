#include "sketch/recovery_discovery.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() / ("property-discovery-" + make_stable_id());
    TemporaryDirectory() { fs::create_directory(path); }
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
ProjectArchiveSnapshot archive(const DocumentSnapshot& document, const std::string& id,
                               std::optional<std::string> source, std::optional<std::string> hash,
                               ArchiveRole role = ArchiveRole::recovery_copy) {
    ProjectWorkspace workspace(document);
    const auto snapshot = workspace.capture();
    const auto history = capture_workspace_history_record(snapshot);
    RecoveryLedger rows{{"history", "workspace_history", encode_workspace_history_record(document, history, {})}};
    if (role == ArchiveRole::recovery_copy) {
        RecoveryCopyRecord record;
        record.archive_id = id;
        record.owner_token = "test-owner";
        record.document_id = document.document_id();
        record.source_path = std::move(source);
        record.source_sha256 = std::move(hash);
        record.explicitly_saved_document_revision = document.saved_revision_optional();
        record.workspace_epoch = history.workspace_epoch;
        record.edited_generation = history.edited_generation;
        record.checkpoint_generation = history.checkpoint_generation;
        rows.push_back({"copy", "recovery_copy", encode_recovery_copy_record(record)});
    }
    return {document, std::move(rows), role};
}
void discovery() {
    TemporaryDirectory temporary;
    const auto source = temporary.path / "source.bldproj";
    const auto directory = temporary.path / "recovery";
    fs::create_directory(directory);
    auto document = Document::create();
    const auto receipt = ProjectStore::save(source, document.snapshot());
    document.mark_saved(receipt.revision);
    const auto snapshot = document.snapshot();
    const auto source_path = utf8(source);
    const auto valid = archive(snapshot, "valid-id", source_path, receipt.file_sha256);
    const auto valid_path = directory / "recovery-a.bldproj";
    const auto recovery_receipt = ProjectStore::save_archive(valid_path, valid);
    (void)ProjectStore::save_archive(directory / "recovery-b.bldproj",
        archive(snapshot, "mismatch-id", source_path, std::string(64, '0')));
    (void)ProjectStore::save_archive(directory / "recovery-c.bldproj",
        archive(snapshot, "ordinary", {}, {}, ArchiveRole::ordinary));
    { std::ofstream corrupt(directory / "recovery-d.bldproj", std::ios::binary); corrupt << "corrupt archive"; }
    (void)ProjectStore::save_archive(directory / "recovery-e.bldproj", archive(snapshot, "duplicate", source_path, receipt.file_sha256));
    (void)ProjectStore::save_archive(directory / "recovery-f.bldproj", archive(snapshot, "duplicate", source_path, receipt.file_sha256));
    (void)ProjectStore::save_archive(directory / "recovery-g.bldproj", archive(snapshot, "traversal", "../source.bldproj", receipt.file_sha256));
    fs::copy_file(valid_path, directory / "ignored.bldproj");
    fs::copy_file(valid_path, directory / "recovery-ignored.bldproj.backup");
    fs::create_directory(directory / "recovery-directory.bldproj");
    const auto nested = directory / "nested";
    fs::create_directory(nested);
    fs::copy_file(valid_path, nested / "recovery-nested.bldproj");
    std::error_code link_error;
    fs::create_symlink(valid_path, directory / "recovery-link.bldproj", link_error);
    if (!link_error) {
        require(!discover_recovery_copies(directory / "recovery-link.bldproj", directory).source_diagnostic.empty(),
                "linked source accepted");
    }
    const auto found = discover_recovery_copies(source, directory);
    require(found.directory_diagnostic.empty() && found.source_diagnostic.empty(), "valid paths rejected");
    require(found.source_sha256 == receipt.file_sha256 && found.candidates.size() == 7, "candidate filtering or source hash failed");
    const auto& a = found.candidates[0];
    require(a.path == valid_path && a.loadable && a.metadata && a.metadata->archive_id == "valid-id" &&
            a.metadata->document_id == snapshot.document_id() && a.metadata->source_path == source_path &&
            a.metadata->source_sha256 == receipt.file_sha256 &&
            a.metadata->explicitly_saved_document_revision == snapshot.saved_revision_optional() &&
            a.file_sha256 == recovery_receipt.file_sha256 && a.source_match == RecoverySourceMatch::matched,
            "valid candidate metadata and match");
    require(!recovery_candidate_has_unsaved_work(a),
            "a recovery copy at its saved generation must not trigger startup recovery");
    auto edited = a;
    edited.metadata->edited_generation = edited.metadata->saved_edited_generation + 1;
    require(recovery_candidate_has_unsaved_work(edited),
            "a recovery copy ahead of its saved generation must trigger startup recovery");
    auto checkpoint_only = a;
    checkpoint_only.metadata->checkpoint_generation =
        checkpoint_only.metadata->autosaved_checkpoint_generation + 1;
    require(recovery_candidate_has_unsaved_work(checkpoint_only),
            "a recovery copy with an unsaved pointer checkpoint must trigger startup recovery");
    auto rejected = edited;
    rejected.loadable = false;
    require(!recovery_candidate_has_unsaved_work(rejected),
            "unsupported recovery candidates must never trigger startup recovery");
    require(found.candidates[1].loadable && found.candidates[1].source_match == RecoverySourceMatch::hash_mismatch,
            "hash mismatch must not match source");
    for (std::size_t i : {2U, 3U})
        require(!found.candidates[i].loadable && !found.candidates[i].reason.empty(), "unsupported/corrupt candidate admitted");
    for (std::size_t i : {4U, 5U})
        require(found.candidates[i].duplicate_archive_id && !found.candidates[i].loadable &&
                found.candidates[i].metadata->archive_id == "duplicate", "duplicate identity not retained and blocked");
    require(found.candidates[6].source_match == RecoverySourceMatch::invalid_source_path, "metadata traversal matched");
    require(ProjectStore::file_sha256(source) == receipt.file_sha256 &&
            ProjectStore::file_sha256(valid_path) == recovery_receipt.file_sha256, "discovery modified source or recovery");
    const auto unbound = discover_recovery_copies({}, directory);
    require(unbound.candidates[0].source_match == RecoverySourceMatch::not_requested, "unbound discovery fabricated source match");
    const auto absent = discover_recovery_copies(temporary.path / "missing.bldproj", directory);
    require(!absent.source_diagnostic.empty() && absent.candidates[0].source_match == RecoverySourceMatch::source_unavailable,
            "missing source treated as a match");
    const auto different = temporary.path / "different.bldproj";
    fs::copy_file(source, different);
    require(discover_recovery_copies(different, directory).candidates[0].source_match == RecoverySourceMatch::different_path,
            "same bytes at another path matched");
    require(!discover_recovery_copies(source, directory / ".." / "recovery").directory_diagnostic.empty(), "directory traversal accepted");
    require(!discover_recovery_copies(source, valid_path).directory_diagnostic.empty(), "file accepted as directory");
    const auto missing = discover_recovery_copies({}, temporary.path / "absent-directory");
    require(missing.candidates.empty() && missing.directory_diagnostic.empty(), "missing recovery directory not empty");
    const auto unassociated = temporary.path / "unassociated";
    fs::create_directory(unassociated);
    (void)ProjectStore::save_archive(unassociated / "recovery-new.bldproj", archive(snapshot, "new", {}, {}));
    require(discover_recovery_copies(source, unassociated).candidates[0].source_match == RecoverySourceMatch::missing_provenance,
            "missing provenance fabricated a source match");
    const auto bounded = temporary.path / "bounded";
    fs::create_directory(bounded);
    for (unsigned i = 0; i < 4097; ++i) fs::create_directory(bounded / std::to_string(i));
    const auto over_budget = discover_recovery_copies({}, bounded);
    require(over_budget.candidates.empty() && !over_budget.directory_diagnostic.empty(), "entry budget did not fail closed");
}
}  // namespace
int main() {
    sketch::testing::noninteractive_errors();
    try { discovery(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
