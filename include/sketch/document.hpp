#pragma once
#include "sketch/boundary_identity_history.hpp"
#include "sketch/geometry.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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

struct BoundaryTransformation {
    std::string boundary_id;
    PlanarTransform transform;
};

struct TransformBoundary {
    Revision expected_revision = 0;
    BoundaryTransformation transformation;
};

using Command = std::variant<ApplyEntityChanges, NameRevision, TranslateBoundary, TransformBoundary>;

// Commands cross worker, workspace, and persistence boundaries as a strict,
// versioned JSON envelope.  The codec preserves typed command identity and
// exact entity/asset payloads; decoding performs structural validation before
// a caller is allowed to apply the command to a live document.
[[nodiscard]] nlohmann::json command_to_json(const Command& command);
[[nodiscard]] Command command_from_json(const nlohmann::json& value);

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
};

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

private:
    friend class Document;
    friend class ProjectStore;
    friend class ProjectStoreAccess;

    DocumentSnapshot() = default;

    std::string document_id_;
    Revision revision_ = 0;
    std::optional<Revision> saved_revision_;
    bool editable_ = true;
    std::string read_only_reason_;
    std::vector<RevisionRecord> history_;
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
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
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
    std::map<std::string, Revision, std::less<>> named_revisions_;
    std::optional<std::string> unsupported_constraint_history_reason_;
    std::optional<std::string> session_read_only_reason_;
    BoundaryIdentityHistory boundary_identity_history_;
};

}  // namespace sketch
