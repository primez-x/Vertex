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
    // proofs require v7; boundary constraint transactions require v8; atomic
    // boundary translation groups require v9. Curved wall proofs and known
    // constraints bound to curved segments require v10; straight wall-only
    // endpoint command proofs require v11; physical arc-length constraints
    // require v12; direct physical curve-length proofs and their known
    // exact-input receipts require v13; version-two curve construction archives
    // containing independently replayed rigid transforms require v14. Fixed-chord
    // boundary curvature reconstruction proofs require v15; reviewed exterior
    // source replacement and identified topology origins require v16; explicit
    // fresh topology redefinition requires v17; grouped rigid transforms
    // require v18; live exterior source completion requires v19; mixed source
    // completion with supplemental entities or assets requires v20; retained
    // ANSI-oriented appraisal policies or measured evidence require v21;
    // physical exterior corner inverse proofs require v22; explicit automatic
    // angle removal during redraw requires v23; explicit saved-view drawing
    // appearance requires v24; explicit SVG instance colors require v25;
    // compact mixed asset references require v26; verified connected wall
    // rigid-transform proofs require v27; schema/replay-two measured linework
    // and identified measured areas retaining linework sources require v28;
    // schema/replay-three measured stroke edit derivations require v29;
    // explicitly reviewed measured-area source replacements require v30;
    // durable typed chord inputs and their retained command proofs require v31;
    // the finished-room appraisal rule and explicit room confirmation require v32;
    // grouped measured-region source evidence requires v33, including opaque markers;
    // curved survey source provenance requires v34, including historical/future markers;
    // measured replay5/command11 and constraint bindings require v35;
    // saved dimensions targeting measured strokes require v36 across retained history.
    // Any may include
    // the optional recovery ledger. Absent proofs preserve historical formats.
    // Wall split authority, arc-chain relations and whole-span dimensions require v37.
    // Schema-two whole-wall relationship membership, including future opaque
    // relationship models, requires v38 across every retained revision.
    // Typed saved-callout placement completion requires v41 across retained history.
    // Source-bound physical rooms require v43 across retained history.
    // Reviewed same-ID physical room repairs and their entity-only derivation
    // carriers require v44, including deleted owners and abandoned history.
    // Annotation-v8 alignment and independent live area-callout roles require
    // v45 anywhere in retained history, including undone/deleted presentation.
    // Canonical v3 landing railings require v54; analytical physical-wall
    // axis dimensions require v55, including deleted and undone history.
    // Form-specific appraisal reporting and typed limitation declarations
    // require v56 across current, deleted and undone history.
    // Independent/nested assembly geometry, joined roof material ownership and
    // per-unit appraisal reporting require v57 across all retained revisions.
    // Explicit property/building/terrain site frames, non-container
    // presentation frames and framed annotation entities require v58 across
    // current, deleted, undone and abandoned revisions, including future
    // opaque marker forms.
    // Atomic DISTO attachments retain envelope-nineteen geometry and observation
    // proofs across current, deleted and undone history and require v59.
    // Annotation-v10 model-plan symbol anchors require v60 across every retained
    // revision, including deleted, undone and abandoned symbol presentation.
    // Sheet/view model v8 annotations in plans/elevations require v61,
    // including when retained only in history or a deleted view graph.
    // Curved sloped walls, explicit retained top planes and straight rigid
    // wall proofs require v62 across every retained revision and receipt.
    // Per-flight stair dimensions require v63, including retained history.
    // Source-reconstructed wall merges and retained merge archives require v64.
    // Room-aware wall splits and their entity-only derivations require v65
    // across every retained revision, including deleted and abandoned owners.
    // Qualified mixed-stroke replay and saved wall dimensions require v66,
    // including commands retained after undo or deletion.
    // Explicit measured-copy source isolation requires v67, including when
    // its owner survives only in undone, deleted or abandoned history.
    // Assembly transform reflection parity and right-aligned straight stair
    // connections require v68 throughout current and retained history.
    // Combined live area-callout rotation requires v69, including when only
    // deleted, undone or abandoned annotation records retain that placement.
    // Per-target joint presentation movement requires v70, including an
    // undone or abandoned command whose explicit target list is empty.
    // Atomic connected geometry/architectural selection completion requires v71.
    // Per-owner connected translations require v72 in all retained history.
    // Per-owner rigid geometry groups require v73 in all retained history.
    // Per-owner connected rigid operations require v74 in all retained history.
    // Explicit connected curve construction requires v75 in all retained history.
    // Reviewed curve/physical-room composition requires v76 in all retained history.
    // Ordinary physical-wall/room composition requires v77 in all retained history.
    // Reviewed wall-profile changes require v78 in all retained history.
    // Context/plane room review requires v81; grouped wall deletion requires v82.
    // Intact joint wall commands composed with room review require v83.
    // Baseline-preserving phase variants and active-phase room review require
    // v84 in all retained history, including terrain phase membership.
    // Reviewed proposed-room redefinition/retirement requires v85 throughout history.
    // Source-bound active-design constraint authoring requires v86, including
    // wrapped room/selection commands and an undone or abandoned edit.
    // Complete mixed-wall removal with manufactured opening-hosted components
    // requires v142 throughout retained single and batch room-review history.
    // Explicit proposed-railing rehost/retirement in stair replacements
    // requires v143 throughout direct, coordinated and wrapped history.
    // Mixed wall and independently selected opening removal with room review
    // requires v144 in current and all retained command history.
    // Independent drawing removal around exact wall/room review requires v145,
    // including wrapped, retained and undone command history.
    // Direct phase authoring enclosed by independent drawing removal requires
    // v146 throughout retained and undone history (outer command version 42).
    // Full baseline wall demolition authoring requires v147, including wrapped,
    // retained and undone command history (phase authoring version 16).
    // Ordinary wall removal alongside baseline wall demolition requires v148,
    // including wrapped, retained and undone history (wall demolition version 2).
    // Complete coordinated hosted-catalog consequences require v149 throughout
    // retained and undone history (phase authoring v15, coordinated demolition v4).
    // Independent ordinary openings in complete coordinated demolition require
    // v150 throughout retained/undone history (coordinated demolition v5).
    // Complete proposed-roof catalog consequences require v151 throughout
    // retained/undone history (coordinated demolition v6).
    // Complete mixed wall/opening/roof catalog consequences require v152
    // throughout retained/undone history (wall demolition authoring inner v3).
    // Independent complete placed-component consequences require v153
    // throughout retained/undone history (coordinated demolition inner v7).
    // Overhead tilt-up door operation v3 requires v154, including retained
    // and undone entities; older readers must not reinterpret its pose.
    // Explicit passage frames and atomic implicit-door overhead conversion
    // require v155, including retained/undone typed edit history.
    // Phase-aware physical-room wall split/merge intents and continuation
    // receipt v2 require v156, including wrapped, retained and undone history.
    // Current physical-room callout joint dialects 5..8 require v157.
    // Reviewed selected physical-room callout placement intents v4 require v158.
    // Physical-room reviews retaining a complete mixed selection require v159.
    // Fresh design-set imports retaining active-phase constraint policy require v160.
    // Immutable DXF reconstruction receipts and shared content-addressed asset
    // payloads require v161; asset identity and metadata remain revision-local.
    // Histories without receipts or sharing retain their
    // usual semantic format floor and legacy inline BLOB schema.
    // Explicit relationship endpoint retargets in physical-room review v5
    // require v162, including nested proofs and retained Undo branches.
    // Roof-edit v6 and roof-replacement v9 coordinate actual hosted components
    // and require v163, including nested proofs and retained Undo branches.
    // Compound stair edit v2 coordinates type-owned hosted profiles after a
    // vertical binding edit; nested and undone proofs require reader 164.
    // Door operation v4 and explicit barn/pocket/bifold conversion v3 require
    // v165 throughout retained entities and nested/undone command proofs.
    // v166 carries five-pane projecting bow-window profiles, including retained
    // entities and nested/undone command proofs.
    // v167 adds top-hinged awning and independent upper/lower double-hung
    // opening profiles throughout retained and nested/undone history.
    // v168 adds roof-hosted skylights, explicit roster/composite proofs and
    // schema-aware scale/resize archives throughout retained history.
    // v169 adds explicit lossless roof-opening transfer authority in retained,
    // nested and undone history; existing schema-three roof values stay v168.
    // v170 adds a corner-window owner with two coordinated hosted cut entities.
    // v171 adds source-derived corner-window cohorts and profile edits in saved
    // alternatives through wall-replacement leaf eight; legacy leaves retain
    // their exact codecs and admission behavior.
    // v172 adds persistent corner-window leg targets and aligned linked-view
    // dimensions (sheet/view schema 9), including retained/undone history.
    // v173 adds independently replayed physical wall-group scaling, its room
    // review enclosure and ordinary active-design scale intent in retained history.
    // v174 adds source-derived baseline wall-group scale replacements within
    // active alternatives, including reviewed and undone phase intent v19.
    // v175 adds roof-face skylight rotation and its explicit retained edit,
    // replacement and transform dialects without activating legacy row fields.
    // v176 retains atomic ordinary-object/skylight removal with complete
    // captured and detached-stage history authority under command envelope45.
    // v177 adds closed phase-demolition ordinary selection to mixed removal,
    // preserving both original and staged phase destinations through history.
    // v178 adds complete hosted-opening and wall/room-review ordinary deletion
    // to mixed skylight commands, preserving explicit roots and typed proofs.
    // v179 retains complete phase/drawing deletion and older wall-room reviews
    // with explicit source roots, including mixed and undone history.
    // v180 adds explicit coordinated corner removal/demolition to mixed
    // selection, retaining actual owners and complete staged history.
    // v181 fences version-two corner catalog-host admission in mixed dialect six.
    // v183 fences complete wall/corner retirement and baseline demolition.
    // v184 fences actual phase corner cohorts, including explicit corner roots.
    // v185 retains atomic mixed clipboard placement and its original-source proof.
    // v186 retains explicit managed corner-cut selection without owner promotion.
    // v187 retains source-reconstructed mixed ordinary and roof-child editing.
    // v188 retains paired mixed/ordinary dialect two presentation similarity.
    // v189 retains explicit rigid two-host corner-window callout completion.
    // v190 retains mixed selected geometry with complete typed room review.
    static constexpr std::uint32_t format_version = 190;
    static constexpr std::uint32_t recovery_format_version = 4;
    [[nodiscard]] static std::uint32_t required_format_version(const DocumentSnapshot& snapshot);
    static constexpr std::uint64_t maximum_file_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t maximum_revision_count = 10'000;
    static constexpr std::uint64_t maximum_entity_rows = 250'000;
    static constexpr std::uint64_t maximum_asset_rows = 100'000;
    static constexpr std::uint64_t maximum_encoded_json_bytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t maximum_json_values = 2'000'000;
    // Physical unique payload bytes in v161; aggregate revision BLOB bytes in
    // older formats. Transient buffers, metadata and replay have separate limits.
    static constexpr std::uint64_t maximum_total_asset_bytes = 512ULL * 1024ULL * 1024ULL;

    [[nodiscard]] static SaveReceipt save(const std::filesystem::path& destination,
                                          const DocumentSnapshot& snapshot,
                                          const SaveOptions& options = {});
    [[nodiscard]] static LoadResult load(const std::filesystem::path& source);
    // Recovery-bearing v4 through the current format. Document-only paths never drop a ledger.
    [[nodiscard]] static SaveReceipt save_archive(const std::filesystem::path& destination,
        const ProjectArchiveSnapshot&, const SaveOptions& options = {});
    [[nodiscard]] static ArchiveLoadResult load_archive(const std::filesystem::path& source, ArchiveRole role);
    [[nodiscard]] static std::string file_sha256(const std::filesystem::path& source);
};

}  // namespace sketch
