#pragma once
#include "sketch/boundary_edit.hpp"
#include "sketch/boundary_identity_history.hpp"
#include "sketch/stair_identity_history.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"
#include "sketch/field_adapter_contract.hpp"
#include "sketch/physical_room_split_ids.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sketch {

using Revision = std::uint64_t;
class ProjectStoreAccess;

std::string make_stable_id();
std::string sha256_hex(std::span<const std::byte> bytes);
bool is_known_entity_type(std::string_view type) noexcept;

struct Entity {
    std::string id;
    std::string type;
    nlohmann::json properties = nlohmann::json::object();
    bool required = false;
    nlohmann::json extensions = nlohmann::json::object();

    static Entity create(std::string type,
                         nlohmann::json properties = nlohmann::json::object(),
                         bool required = false,
                         nlohmann::json extensions = nlohmann::json::object());

    bool operator==(const Entity&) const = default;
};

struct Asset {
    std::string id;
    std::string media_type;
    std::vector<std::byte> bytes;
    std::string sha256;
    nlohmann::json metadata = nlohmann::json::object();

    static Asset create(std::string id, std::string media_type, std::vector<std::byte> bytes,
                        nlohmann::json metadata = nlohmann::json::object());
    static Asset create(std::string media_type, std::vector<std::byte> bytes,
                        nlohmann::json metadata = nlohmann::json::object());

    bool operator==(const Asset&) const = default;
};

enum class EntityChangeKind { upsert, erase };

struct EntityChange {
    EntityChangeKind kind = EntityChangeKind::upsert;
    Entity entity;
    std::string entity_id;

    static EntityChange upsert(Entity entity);
    static EntityChange erase(std::string entity_id);
};

enum class AssetChangeKind { upsert, erase };

struct AssetChange {
    AssetChangeKind kind = AssetChangeKind::upsert;
    Asset asset;
    std::string asset_id;

    static AssetChange upsert(Asset asset);
    static AssetChange erase(std::string asset_id);
};

struct ApplyEntityChanges {
    Revision expected_revision = 0;
    std::vector<EntityChange> entity_changes;
    std::vector<AssetChange> asset_changes;
    std::string message;
};

struct NameRevision {
    Revision expected_revision = 0;
    std::string name;
};

struct BoundaryTranslation {
    std::string boundary_id;
    Vec2 offset;
};

struct TranslateBoundary {
    Revision expected_revision = 0;
    BoundaryTranslation translation;
};

// Qualified translations and their supplemental entity changes are admitted
// together after every measured owner has been reconstructed.
struct TranslateBoundaries {
    Revision expected_revision = 0;
    std::vector<BoundaryTranslation> translations;
    std::vector<EntityChange> entity_changes;
    std::string message;
};

struct BoundaryTransformation {
    std::string boundary_id;
    PlanarTransform transform;
};

struct TransformBoundary {
    Revision expected_revision = 0;
    BoundaryTransformation transformation;
};

// One shared rigid transform and its supplemental physical/relationship edits
// are admitted only after every measured owner has been reconstructed.
struct TransformBoundaries {
    Revision expected_revision = 0;
    std::vector<BoundaryTransformation> transformations;
    std::vector<EntityChange> entity_changes;
    std::string message;
};

struct EditBoundaryGeometry {
    Revision expected_revision = 0;
    BoundaryGeometryEdit edit;
    // A physical-wall room repair is admitted only in this same-ID typed
    // command, independently rederived from the preceding entity state.
};

struct ConstraintWallGeometryEdit {
    std::string wall_id;
    Segment baseline;
    std::optional<Quantity> length_entry;
    // Version one is the historical straight edit. Version two preserves
    // an existing signed curve sweep during endpoint-coordinate editing.
    // Version three records a physical curve-length entry at that fixed sweep.
    std::uint64_t version{1};
    // Version four independently reconstructs a selected curved wall's rigid
    // motion. Version five reconstructs a selected straight wall's rigid motion
    // and explicit top-plane basis. Dependent endpoint edits retain versions one
    // through three.
    std::optional<PlanarTransform> rigid_transform;
};

// An exterior analytical corner edit is replayed through the physical source
// walls, never through an independently editable measured-outline payload.
struct ExteriorCornerMoveIntent {
    std::string boundary_id;
    std::string vertex_id;
    Vec2 target_position;
    bool move_connected_objects{true};
};

// Resize a selected measured exterior edge through its physical source walls.
// The exact entry and both movement choices form independently replayed authority.
struct ExteriorSegmentResizeIntent {
    std::string boundary_id;
    std::string segment_id;
    Quantity exact_length;
    BoundaryFixedEndpoint fixed_endpoint{BoundaryFixedEndpoint::start};
    bool move_boundary_chain{};
    bool move_connected_objects{true};
};

// Reconstruct one measured exterior curve from its exact entered chord receipt.
// Both measured chord endpoints remain fixed; physical wall geometry is derived.
struct ExteriorSegmentArcIntent {
    std::string boundary_id;
    std::string segment_id;
    ConstructionReceipt arc_construction;
    bool move_connected_objects{true};
};

struct WallSplitMeasuredOwnerIds {
    std::string boundary_id;
    std::string vertex_id;
    std::string segment_id;
    std::string automatic_dimension_id;
};

// One source-reconstructed physical partition. No entity payload can lend
// authority to a split or to its dependent measured geometry.
struct WallSplitIntent {
    std::string wall_id;
    std::string second_wall_id;
    double fraction{};
    std::string seam_constraint_id;
    std::vector<WallSplitMeasuredOwnerIds> measured_owners;
    // Nested version two retains every initially-current clear-room owner.
    // The same rooms survive; only frozen analytical child IDs may be new.
    bool physical_room_completion{};
    std::vector<WallSplitPhysicalRoomIds> physical_room_owners;
};

// One source-reconstructed directed physical union. The first wall survives;
// the second and its retired analytical identities stay reserved in history.
struct WallMergeIntent {
    std::string first_wall_id;
    std::string second_wall_id;
};

// One translation across selected rigid owners and selected physical walls.
// All selected points are exact targets in one connected constraint solve;
// existing fixed anchors never move merely because their owner is selected.
struct JointTranslationIntent {
    Vec2 offset;
    std::vector<std::string> rigid_boundary_ids;
    std::vector<std::string> rigid_stroke_ids;
    std::vector<std::string> partial_wall_ids;
    bool move_connected_objects{true};
    // Independent selected saved callouts follow the offset once. Callouts
    // owned by a selected rigid owner already follow that owner's provenance.
    std::vector<std::string> dimension_ids;
    // Optional view-XY displacement for symbols, overlay labels and references.
    // Model-plan labels and analytical geometry retain offset in world XY.
    std::optional<Vec2> presentation_offset;
};

// One observation of an existing owner's supported field. This carries no
// entity payload and cannot authorize geometry or other metadata changes.
struct DistoMeasurementAttachment {
    std::string owner_id;
    DistoMeasurementRecord record;
    bool replace_existing{};
};

// Geometry is replayed before the relation changes are validated.
// The original entity_changes lane contains only constraints. Version six's
// physical lane admits existing-wall changes under ordinary provenance rules;
// boundary payloads never substitute for typed, receipt-preserving edits.
struct ApplyBoundaryConstraintChanges {
    Revision expected_revision = 0;
    std::vector<BoundaryGeometryEdit> boundary_edits;
    std::vector<EntityChange> entity_changes;
    std::string message;
    // Mixed straight wall/boundary edits use envelope version 2; any
    // version-two curved wall proof requires envelope version 3. Straight
    // wall-only edits use version 4. A version-three physical curve-length
    // proof requires envelope version 5. Boundary-only stays version 1.
    std::vector<ConstraintWallGeometryEdit> wall_edits;
    // Version six additionally admits existing-wall physical/construction changes
    // and verified exterior redraws, in that order after primary geometry and
    // constraints. Exterior updates are recomputed from the original source;
    // they cannot stand in for arbitrary raw boundary payloads.
    std::vector<EntityChange> physical_entity_changes;
    std::vector<BoundaryGeometryEdit> exterior_source_edits;
    // Retains envelope version six even if a caller removes its edit vectors.
    bool exterior_source_completion{};
    // Version seven retains ordinarily admitted wall/object metadata and asset edits
    // in the same source-completion event. Raw measured owners and dimensions
    // cannot borrow the exterior redraw's typed authority.
    std::vector<EntityChange> supplemental_entity_changes;
    std::vector<AssetChange> supplemental_asset_changes;
    // Retains version seven even if its supplemental vectors are removed.
    bool supplemental_source_completion{};
    // Envelope version eight independently reconstructs the complete physical
    // change from this intent before validating its exact exterior redraws.
    std::optional<ExteriorCornerMoveIntent> exterior_corner_move;
    // Version nine retains compact supplemental asset references, including
    // when the reference list is emptied. Earlier inline proofs keep version seven.
    bool supplemental_asset_reference_completion{};
    // Retains envelope ten even if its selected rigid wall proofs are removed.
    bool rigid_wall_transform_completion{};
    // Envelope eleven independently replays measured-stroke intent, then
    // rebuilds eligible current source-derived areas before final validation.
    // Raw entity payloads never authorize changing measured geometry.
    struct MeasuredStrokeEdit {
        std::string stroke_id;
        std::optional<BoundaryGeometryEdit> authored_edit;
        std::optional<Quantity> authored_length;
        std::optional<PlanarTransform> rigid_transform;
        std::vector<BoundaryGeometryEdit> vertex_edits;
    };
    std::vector<MeasuredStrokeEdit> measured_stroke_edits;
    // Retain envelope eleven even for relation-only stroke commands and when
    // the stroke-edit vector is emptied. Other completion modes are separate.
    bool measured_source_completion{};
    // Envelope twelve admits only this source-reconstructed split intent.
    std::optional<WallSplitIntent> wall_split;
    // Envelope thirteen retains source-reconstructed measured-edge resize authority.
    std::optional<ExteriorSegmentResizeIntent> exterior_segment_resize;
    // Envelope fourteen independently reconstructs endpoint-fixed curvature.
    std::optional<ExteriorSegmentArcIntent> exterior_segment_arc;
    // Envelope fifteen places existing saved callouts after all typed geometry
    // and automatic reflow. Offsets use the original source text position.
    struct DimensionPlacementMove {
        std::string dimension_id;
        Vec2 offset;
    };
    std::vector<DimensionPlacementMove> dimension_placement_moves;
    // Retain the dialect even if a caller strips the placement lane.
    bool dimension_placement_completion{};
    // Envelope sixteen composes a disjoint rigid group with connected edits.
    // Both typed lanes replay against the same original source before merging.
    std::optional<TransformBoundaries> rigid_group_transform;
    // Retain the dialect even if a caller strips its rigid proof.
    bool rigid_group_completion{};
    // Envelope seventeen replays one connected solve from selected source IDs.
    // The independent envelope-sixteen lanes retain their disjoint contract.
    std::optional<JointTranslationIntent> joint_translation;
    bool joint_translation_completion{};
    // Envelope eighteen admits complete reviewed room dispositions only.
    // Geometry, new identities and reference changes are reconstructed from
    // this semantic intent; ordinary entity payloads cannot lend authority.
    nlohmann::json room_review_intent=nullptr;
    bool room_review_completion{};
    // Envelope nineteen replays the complete preceding dialect before attaching
    // exactly this observation. The source owner and completed field must agree.
    std::optional<DistoMeasurementAttachment> disto_measurement;
    bool disto_measurement_completion{};
    // Envelope twenty admits only this source-reconstructed merge intent.
    std::optional<WallMergeIntent> wall_merge;
};

using Command = std::variant<ApplyEntityChanges, NameRevision, TranslateBoundary,
                             TransformBoundary, EditBoundaryGeometry, ApplyBoundaryConstraintChanges,
                             TranslateBoundaries, TransformBoundaries>;

// Commands cross worker, workspace, and persistence boundaries as a strict,
// versioned JSON envelope.  The codec preserves typed command identity and
// exact entity/asset payloads; decoding performs structural validation before
// a caller is allowed to apply the command to a live document.
[[nodiscard]] nlohmann::json command_to_json(const Command& command);
[[nodiscard]] Command command_from_json(const nlohmann::json& value,
    const std::function<const Asset*(std::string_view)>& asset_resolver = {});

enum class DocumentErrorCode {
    stale_revision,
    read_only,
    invalid_entity,
    invalid_asset,
    dangling_reference,
    duplicate_change,
    duplicate_revision_name,
    no_undo,
    no_redo,
    invalid_saved_revision,
    invalid_history,
    constraint_violation,
};

class DocumentError final : public std::runtime_error {
public:
    DocumentError(DocumentErrorCode code, std::string message);
    [[nodiscard]] DocumentErrorCode code() const noexcept;

private:
    DocumentErrorCode code_;
};

struct RevisionRecord {
    Revision revision = 0;
    std::optional<Revision> parent_revision;
    std::optional<Revision> source_revision;
    std::string action;
    std::optional<std::string> name;
    std::map<std::string, Entity, std::less<>> entities;
    std::map<std::string, Asset, std::less<>> assets;
    std::vector<Revision> undo_stack;
    std::vector<Revision> redo_stack;
    std::optional<BoundaryTranslation> boundary_translation;
    std::optional<BoundaryTransformation> boundary_transform;
    std::optional<BoundaryGeometryEdit> boundary_geometry_edit;
    std::optional<ApplyBoundaryConstraintChanges> boundary_constraint_changes;
    std::optional<TranslateBoundaries> boundary_translations;
    std::optional<TransformBoundaries> boundary_transforms;
};

namespace test { class DetachedDocumentSnapshotFixture; }

class DocumentSnapshot {
public:
    DocumentSnapshot(const DocumentSnapshot&) = default;
    DocumentSnapshot& operator=(const DocumentSnapshot&) = default;
    DocumentSnapshot(DocumentSnapshot&&) noexcept = default;
    DocumentSnapshot& operator=(DocumentSnapshot&&) noexcept = default;

    [[nodiscard]] const std::string& document_id() const noexcept;
    [[nodiscard]] Revision revision() const noexcept;
    [[nodiscard]] std::optional<Revision> saved_revision_optional() const noexcept;
    [[nodiscard]] Revision saved_revision() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool is_editable() const noexcept;
    [[nodiscard]] const std::string& read_only_reason() const noexcept;
    [[nodiscard]] const std::map<std::string, Entity, std::less<>>& entities() const noexcept;
    [[nodiscard]] const std::map<std::string, Asset, std::less<>>& assets() const noexcept;
    [[nodiscard]] const std::vector<RevisionRecord>& history() const noexcept;
    [[nodiscard]] const std::map<std::string, Revision, std::less<>>&
    named_revisions() const noexcept;

    // Sufficient authoring-source equality; saved/editability metadata is excluded.
    // This does not replace full snapshot equality for workspace publication.
    [[nodiscard]] bool shares_authoring_source_with(const DocumentSnapshot& other) const noexcept;

    // Sufficient complete snapshot equality, including save/editability state.
    // Independent immutable histories still need full comparison or a digest;
    // matching document identity and revision alone never satisfy this proof.
    [[nodiscard]] bool shares_full_snapshot_with(const DocumentSnapshot& other) const noexcept;

private:
    friend class Document;
    friend class test::DetachedDocumentSnapshotFixture;
    friend class ProjectStore;
    friend class ProjectStoreAccess;

    DocumentSnapshot() = default;

    std::string document_id_;
    Revision revision_ = 0;
    std::optional<Revision> saved_revision_;
    bool editable_ = true;
    std::string read_only_reason_;
    std::shared_ptr<const std::vector<RevisionRecord>> history_;
    std::map<std::string, Revision, std::less<>> named_revisions_;
};

class Document {
public:
    static Document create();
    static Document create(std::vector<Entity> initial_entities,
                           std::vector<Asset> initial_assets = {});

    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document() = default;

    [[nodiscard]] Revision revision() const noexcept;
    [[nodiscard]] std::optional<Revision> saved_revision_optional() const noexcept;
    [[nodiscard]] Revision saved_revision() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool is_editable() const noexcept;
    [[nodiscard]] const std::string& read_only_reason() const noexcept;
    [[nodiscard]] const std::string& document_id() const noexcept;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    // Capture on the owning document thread. Save workers consume immutable
    // snapshots and must not capture or access the editable Document.
    [[nodiscard]] DocumentSnapshot snapshot() const;

    // A private working copy with the same identity and complete validated
    // history. This is not an independent project copy or a way around
    // read-only/history rules; workspace publication still requires its CAS.
    [[nodiscard]] static Document fork(const DocumentSnapshot& source);
    // Validates the complete source, then reconstructs a private retained prefix
    // with its original identity, navigation, names and derived editability.
    // A later save marker is omitted. This does not rebind any live workspace.
    [[nodiscard]] static Document fork_at_revision(const DocumentSnapshot& source, Revision revision);

    // Revalidates the complete captured history, then applies the command to
    // a private document. Neither the source nor any live document is changed.
    // The result is a preview, not authorization to bypass a later revision
    // or source-snapshot check when committing to a live document.
    [[nodiscard]] static DocumentSnapshot preview_command(
        const DocumentSnapshot& source, const Command& command);

    Revision apply(const Command& command);
    Revision undo(Revision expected_revision);
    Revision redo(Revision expected_revision);
    void mark_saved(Revision revision);
    // Latches a session-level read-only reason without changing document
    // history.  Used when an external ownership or integrity condition makes
    // further in-place edits unsafe; Save As can still be offered by a host
    // that creates an explicit independent copy.
    void mark_read_only(std::string reason);

private:
    friend class ProjectStore;
    friend class ProjectStoreAccess;

    static Document restore(DocumentSnapshot snapshot);
    explicit Document(std::string document_id);

    [[nodiscard]] const RevisionRecord& head_record() const;
    void update_editability();

    std::string document_id_;
    Revision head_revision_ = 0;
    std::optional<Revision> saved_revision_;
    bool editable_ = true;
    std::string read_only_reason_;
    std::vector<RevisionRecord> history_;
    mutable std::shared_ptr<const std::vector<RevisionRecord>> snapshot_history_cache_;
    std::map<std::string, Revision, std::less<>> named_revisions_;
    std::optional<std::string> unsupported_constraint_history_reason_;
    std::optional<std::string> session_read_only_reason_;
    BoundaryIdentityHistory boundary_identity_history_;
    StairIdentityHistory stair_identity_history_;
};

// Prepares geometry and its observation against the same immutable source.
// Ordinary commands keep ordinary admission; typed commands retain every proof
// in envelope nineteen. This does not publish or bypass live-source checks.
[[nodiscard]] Command complete_disto_measurement_command(
    const DocumentSnapshot& source, const Command& geometry_command,
    std::string_view owner_id, const DistoMeasurementRecord& record,
    bool replace_existing = false);

}  // namespace sketch
