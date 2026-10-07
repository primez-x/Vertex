#include "support/detached_document_snapshot.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"

#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
using namespace sketch;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}


void test_shared_immutable_snapshot_authority_and_lifetime() {
    Entity label{"shared-label", "label", {{"text", "original"}}, false, nlohmann::json::object()};
    auto document = Document::create({label});
    const auto initial = document.snapshot();
    const auto copied = initial;
    const auto initial_full = document_snapshot_digest(initial);
    require(initial.shares_authoring_source_with(copied) &&
            initial.shares_authoring_source_with(document.snapshot()) &&
            document.document_id() == initial.document_id(),
            "unchanged captures and copies must share immutable authoring history");
    bool rejected = false;
    try { (void)document.undo(document.revision()); }
    catch (const DocumentError&) { rejected = true; }
    require(rejected && initial.shares_authoring_source_with(document.snapshot()),
            "rejected navigation must retain the snapshot cache");
    document.mark_saved(document.revision());
    const auto saved = document.snapshot();
    require(initial.shares_authoring_source_with(saved) &&
            document_snapshot_digest(saved) != initial_full &&
            document_authoring_source_digest_v1(saved) == document_authoring_source_digest_v1(initial),
            "save metadata must remain fresh while authoring history is shared");
    label.properties["text"] = "edited";
    document.apply(ApplyEntityChanges{.expected_revision = 0,
        .entity_changes = {EntityChange::upsert(label)}, .message = "edit"});
    const auto edited = document.snapshot();
    require(!edited.shares_authoring_source_with(initial) &&
            edited.shares_authoring_source_with(document.snapshot()) &&
            initial.entities().at(label.id).properties.at("text") == "original",
            "successful apply must replace the cache without changing old captures");
    rejected = false;
    try { document.apply(NameRevision{0, "stale"}); }
    catch (const DocumentError&) { rejected = true; }
    require(rejected && edited.shares_authoring_source_with(document.snapshot()),
            "rejected commands must keep the same immutable history");
    document.apply(NameRevision{document.revision(), "retained"});
    const auto named = document.snapshot();
    require(!named.shares_authoring_source_with(edited) && edited.named_revisions().empty() &&
            named.named_revisions().at("retained") == named.revision(),
            "naming must replace history and copy fresh named-revision metadata");
    document.undo(document.revision());
    const auto undone = document.snapshot();
    document.redo(document.revision());
    const auto redone = document.snapshot();
    require(!undone.shares_authoring_source_with(named) &&
            !redone.shares_authoring_source_with(undone) &&
            redone.entities() == named.entities(),
            "undo and redo must replace retained history even with equal geometry");
    auto fork = Document::fork(redone);
    require(fork.snapshot().shares_authoring_source_with(redone),
            "fully validated restoration may seed the immutable source cache");
    fork.apply(NameRevision{fork.revision(), "private"});
    require(!fork.snapshot().shares_authoring_source_with(redone) &&
            !redone.named_revisions().contains("private"),
            "fork edits must detach history and names from their source");
    auto prefix = Document::fork_at_revision(redone, edited.revision());
    const auto prefix_snapshot = prefix.snapshot();
    require(!prefix_snapshot.shares_authoring_source_with(edited) &&
            document_snapshot_digest(prefix_snapshot) == document_snapshot_digest(edited),
            "retained prefixes must own separate truncated immutable history");
    document.mark_read_only("session ownership diagnostic");
    const auto read_only = document.snapshot();
    require(read_only.shares_authoring_source_with(redone) && !read_only.is_editable() &&
            document_snapshot_digest(read_only) != document_snapshot_digest(redone),
            "read-only metadata must be copied without invalidating authoring history");
    auto identity_changed = redone;
    const_cast<std::string&>(identity_changed.document_id()) = "other-document";
    auto names_changed = redone;
    const_cast<std::map<std::string, Revision, std::less<>>&>(names_changed.named_revisions())
        .emplace("forged-name", 0);
    require(!identity_changed.shares_authoring_source_with(redone) &&
            !names_changed.shares_authoring_source_with(redone),
            "shared history alone must not bypass identity or named-revision authority");
    const auto lifetime = [&] {
        auto temporary = Document::fork(initial);
        const auto retained = temporary.snapshot();
        auto moved = std::move(temporary);
        moved.apply(NameRevision{moved.revision(), "after-move"});
        require(retained.shares_authoring_source_with(initial),
                "moving and editing the owner must preserve old snapshot storage");
        return retained;
    }();
    require(document_snapshot_digest(lifetime) == initial_full,
            "snapshots must survive owner move and destruction");
    const auto source_digest = document_snapshot_digest(redone);
    const auto historical_forgery = test::DetachedDocumentSnapshotFixture::mutate(redone, [](auto& fixture) {
        fixture.history().front().action = "forged create";
    });
    const auto entity_forgery = test::DetachedDocumentSnapshotFixture::mutate(redone, [](auto& fixture) {
        fixture.entities().at("shared-label").properties["text"] = "forged entity";
    });
    const auto proof_forgery = test::DetachedDocumentSnapshotFixture::mutate(redone, [](auto& fixture) {
        fixture.history().front().boundary_translation = BoundaryTranslation{"forged-boundary", {1, 2}};
    });
    const auto invalid_suffix = test::DetachedDocumentSnapshotFixture::mutate(redone, [](auto& fixture) {
        fixture.history().back().parent_revision = 999;
    });
    for (const auto* forged : {&historical_forgery, &entity_forgery, &proof_forgery}) {
        require(forged->document_id() == redone.document_id() && forged->revision() == redone.revision() &&
                !forged->shares_authoring_source_with(redone) &&
                document_snapshot_digest(*forged) != source_digest,
                "same-ID/revision detached forgeries must fail sufficient source equality");
    }
    for (const auto* invalid : {&historical_forgery, &proof_forgery, &invalid_suffix}) {
        rejected = false;
        try { (void)Document::fork_at_revision(*invalid, 0); }
        catch (const DocumentError&) { rejected = true; }
        require(rejected, "prefix restoration must validate the complete original source");
    }
    test::DetachedDocumentSnapshotFixture writable(initial);
    const auto frozen = writable.freeze();
    writable.entities().at(label.id).properties["text"] = "later fixture edit";
    require(document_snapshot_digest(frozen) == initial_full &&
            document_snapshot_digest(redone) == source_digest &&
            document_snapshot_digest(copied) == initial_full,
            "fixture edits must preserve original and previously frozen captures");
}

void test_full_snapshot_binding() {
    Entity label{"label-1", "label", {{"text", "original"}}, false, nlohmann::json::object()};
    auto asset = Asset::create("asset-1", "application/octet-stream",
                               {std::byte{0}, std::byte{255}, std::byte{12}}, {{"note", "original"}});
    auto document = Document::create({label}, {asset});
    label.properties["text"] = "head";
    document.apply(ApplyEntityChanges{.expected_revision = 0,
        .entity_changes = {EntityChange::upsert(label)}, .message = "advance"});
    const auto source = document.snapshot();
    const auto original_digest = document_snapshot_digest(source);
    const auto original_authoring_digest = document_authoring_source_digest_v1(source);
    require(original_digest.size() == 64 && document_snapshot_digest(source) == original_digest,
            "snapshot digest must be stable SHA256 text");
    using Mutation = std::function<void(RevisionRecord&)>;
    const std::vector<Mutation> mutations{
        [](auto& record) { record.entities.at("label-1").properties["text"] = "changed historical text"; },
        [](auto& record) { record.entities.at("label-1").extensions["opaque"] = true; },
        [](auto& record) { record.entities.at("label-1").required = true; },
        [](auto& record) { record.assets.at("asset-1").bytes[1] = std::byte{254}; },
        [](auto& record) { record.assets.at("asset-1").sha256 = std::string(64, '0'); },
        [](auto& record) { record.assets.at("asset-1").media_type = "image/png"; },
        [](auto& record) { record.assets.at("asset-1").metadata["opaque"] = true; },
        [](auto& record) { record.action = "changed action"; },
        [](auto& record) { record.name = "named history"; },
        [](auto& record) { record.parent_revision = 0; },
        [](auto& record) { record.source_revision = 0; },
        [](auto& record) { record.undo_stack.push_back(0); },
        [](auto& record) { record.redo_stack.push_back(0); },
        [](auto& record) { record.boundary_constraint_changes = ApplyBoundaryConstraintChanges{0,
            {{"boundary-1", BoundaryGeometryEditKind::move_vertex, "vertex-1", {3, 1}}}, {}, "proof"}; },
        [](auto& record) { record.boundary_translation = BoundaryTranslation{"boundary-1", {8, -4}}; },
        [](auto& record) { record.boundary_translations = TranslateBoundaries{0,
            {{"boundary-1", {8, -4}}, {"boundary-2", {8, -4}}},
            {EntityChange::erase("old-label")}, "group proof"}; },
        [](auto& record) { record.boundary_transforms = TransformBoundaries{0,
            {{"boundary-1",{{2,1},.37,true,false,{8,-4}}}}, {}, "rigid group proof"}; },
    };
    for (const auto& mutate : mutations) {
        sketch::test::DetachedDocumentSnapshotFixture changed(source);
        mutate(changed.history().front());
        require(changed.entities() == source.entities() && changed.revision() == source.revision(),
                "history fixture must leave the visible head and revision unchanged");
        require(document_snapshot_digest(changed) != original_digest,
                "snapshot digest missed changed history, navigation or actual asset data");
        require(document_authoring_source_digest_v1(changed) != original_authoring_digest,
                "authoring source missed changed history, navigation or actual asset data");
    }
    require(document_snapshot_digest(source) == original_digest,
            "copied snapshot mutation changed the original snapshot");
    sketch::test::DetachedDocumentSnapshotFixture mismatched_asset(source);
    auto& asset_map = mismatched_asset.history().front().assets;
    auto asset_node = asset_map.extract("asset-1");
    asset_node.key() = "different-key";
    asset_map.insert(std::move(asset_node));
    bool rejected = false;
    try { (void)document_snapshot_digest(mismatched_asset); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "historical asset map identity mismatch was not rejected");
    document.mark_saved(document.revision());
    require(document_snapshot_digest(document.snapshot()) != original_digest,
            "snapshot digest missed saved-state changes");
    auto foreign = Document::create({label}, {asset});
    require(entity_map_digest(foreign.snapshot().entities()) == entity_map_digest(source.entities()) &&
                document_snapshot_digest(foreign.snapshot()) != original_digest,
            "document identity must be bound independently from equal entity content");
}

void test_candidate_maps_preserve_identity_authority() {
    Entity label{"label-1", "label", {{"text", "original"}}, false, nlohmann::json::object()};
    auto document = Document::create({label});
    auto candidate = document.snapshot().entities();
    const auto original = entity_map_digest(candidate);
    candidate.at(label.id).extensions["future_data"] = {{"retain", true}};
    require(entity_map_digest(candidate) != original, "candidate digest missed opaque metadata");
    auto node = candidate.extract(label.id);
    node.key() = "forged-map-key";
    candidate.insert(std::move(node));
    bool rejected = false;
    try { (void)entity_map_digest(candidate); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "candidate map key mismatch was not rejected");
}

void test_authoring_source_survives_save_bookkeeping() {
    Entity label{"label-1", "label", {{"text", "original"}}, false, nlohmann::json::object()};
    auto document = Document::create({label});
    const auto source = document.snapshot();
    const auto binding = document_authoring_source_digest_v1(source);
    const auto full_binding = document_snapshot_digest(source);
    require(binding.size() == 64 && binding != full_binding,
            "authoring source must have a separate versioned digest domain");
    document.mark_saved(document.revision());
    require(document_authoring_source_digest_v1(document.snapshot()) == binding,
            "first save must not make an unchanged unfinished drawing stale");
    require(document_snapshot_digest(document.snapshot()) != full_binding,
            "full commit binding must still detect changed save bookkeeping");
    label.properties["text"] = "edited";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "edit"});
    const auto edited = document_authoring_source_digest_v1(document.snapshot());
    require(edited != binding, "semantic edit must invalidate authoring source");
    document.mark_saved(document.revision());
    require(document_authoring_source_digest_v1(document.snapshot()) == edited,
            "subsequent save must preserve semantic source binding");
    document.undo(document.revision());
    require(document.snapshot().entities() == source.entities() &&
                document_authoring_source_digest_v1(document.snapshot()) != binding,
            "undo to equal geometry must retain changed revision/history authority");
    document.redo(document.revision());
    require(document_authoring_source_digest_v1(document.snapshot()) != edited,
            "redo must retain its new navigation history in the binding");
    auto foreign = Document::create({source.entities().at("label-1")});
    require(document_authoring_source_digest_v1(foreign.snapshot()) != binding,
            "equal content in a different document must not share source authority");
}

void test_digest_format_vectors() {
    Entity label{"label-vector", "label", {{"text", "original"}}, false, nlohmann::json::object()};
    auto asset = Asset::create("asset-vector", "application/octet-stream",
                              {std::byte{0}, std::byte{255}, std::byte{12}},
                              {{"note", "original"}});
    auto snapshot = Document::create({label}, {asset}).snapshot();
    // A copied fixture uses a fixed ID solely to make its hash reproducible.
    const_cast<std::string&>(snapshot.document_id()) = "document-digest-vector-v1";
    require(document_snapshot_digest(snapshot) ==
                "d5890d766aaa40a6bd13e6247ca4db9559ed56ed0ba6c7e6c7a60041e2a06495",
            "full commit digest changed from the frozen pre-recovery implementation");
    require(document_authoring_source_digest_v1(snapshot) ==
                "a3ae00de721ef26c6314d5e921b5cbf4709a071d15b97759ae64bbc3b76a8ce5",
            "persisted authoring-source version-one encoding changed");
    const auto authoring_binding = document_authoring_source_digest_v1(snapshot);
    const_cast<std::string&>(snapshot.read_only_reason()) = "derived diagnostic";
    require(document_authoring_source_digest_v1(snapshot) == authoring_binding,
            "derived read-only explanation must not alter authoring-source content");
    require(document_snapshot_digest(snapshot) !=
                "d5890d766aaa40a6bd13e6247ca4db9559ed56ed0ba6c7e6c7a60041e2a06495",
            "full commit digest must still detect changed derived diagnostics");
}
void test_historical_authoring_bindings() {
    auto document = Document::create();
    std::vector<DocumentSnapshot> baselines{document.snapshot()};
    document.apply(NameRevision{document.revision(), "Initial baseline"});
    baselines.push_back(document.snapshot());
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "Later"}}))},
        .message = "Later edit"});
    baselines.push_back(document.snapshot());
    document.undo(document.revision());
    baselines.push_back(document.snapshot());
    document.redo(document.revision());
    baselines.push_back(document.snapshot());
    document.apply(NameRevision{document.revision(), "Future name"});
    document.mark_saved(document.revision());
    const auto current = document.snapshot();
    for (const auto& baseline : baselines) {
        require(document_authoring_source_digest_v1_at_revision(current, baseline.revision()) ==
                    document_authoring_source_digest_v1(baseline),
                "historical digest must match captured source, excluding later names/history/saves");
    }
    require(document_authoring_source_digest_v1_at_revision(current, current.revision()) ==
                document_authoring_source_digest_v1(current), "head prefix must preserve v1 digest");
    bool rejected = false;
    try { (void)document_authoring_source_digest_v1_at_revision(current, current.revision() + 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "missing historical revision must reject");
    sketch::test::DetachedDocumentSnapshotFixture forged(current);
    forged.history().front().action = "forged";
    require(document_authoring_source_digest_v1_at_revision(forged, 0) !=
                document_authoring_source_digest_v1(baselines.front()),
            "changed retained baseline data must alter its binding");
}

void test_translation_proof_digest_and_codec() {
    sketch::test::DetachedDocumentSnapshotFixture snapshot(Document::create().snapshot());
    auto& proof = snapshot.history().front().boundary_translation;
    proof = BoundaryTranslation{"boundary-1", {8, -4}};
    const auto digest = document_authoring_source_digest_v1(snapshot);
    proof->offset.x = 9;
    require(document_authoring_source_digest_v1(snapshot) != digest,
            "authoring digest must bind the translation offset");
    proof->offset.x = 8;
    proof->boundary_id = "boundary-2";
    require(document_authoring_source_digest_v1(snapshot) != digest,
            "authoring digest must bind the translation target");
    const auto wire = encode_boundary_translation(*proof);
    const auto decoded = decode_boundary_translation(wire);
    require(decoded.boundary_id == proof->boundary_id && decoded.offset.x == 8 && decoded.offset.y == -4,
            "known translation proof must round trip");
    for (const auto invalid : {nlohmann::json{{"version", 1.0}, {"boundary_id", "boundary-1"}, {"offset", {0, 0}}},
                              nlohmann::json{{"version", 1}, {"boundary_id", "bad id"}, {"offset", {0, 0}}},
                              nlohmann::json{{"version", 1}, {"boundary_id", "boundary-1"}, {"offset", {true, 0}}}}) {
        bool rejected = false;
        try { (void)decode_boundary_translation(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "malformed proof must not silently coerce fields");
    }
}
void test_transform_proof_digest_and_codec() {
    sketch::test::DetachedDocumentSnapshotFixture snapshot(Document::create().snapshot());
    auto& proof = snapshot.history().front().boundary_transform;
    const BoundaryTransformation original{"boundary-1", {{1, 2}, 0.4, true, false, {3, 4}}};
    proof = original;
    const auto digest = document_authoring_source_digest_v1(snapshot);
    const auto snapshot_digest = document_snapshot_digest(snapshot);
    const auto wire = encode_boundary_transform(original);
    require(encode_boundary_transform(decode_boundary_transform(wire)) == wire, "transform codec must roundtrip");
    const std::vector<std::function<void(BoundaryTransformation&)>> mutations{
        [](auto& p) { p.boundary_id = "boundary-2"; },
        [](auto& p) { p.transform.pivot.x += 1; },
        [](auto& p) { p.transform.pivot.y += 1; },
        [](auto& p) { p.transform.rotation_radians += 1; },
        [](auto& p) { p.transform.flip_horizontal = false; },
        [](auto& p) { p.transform.flip_vertical = true; },
        [](auto& p) { p.transform.offset.x += 1; },
        [](auto& p) { p.transform.offset.y += 1; }};
    for (const auto& mutate : mutations) {
        proof = original; mutate(*proof);
        require(document_authoring_source_digest_v1(snapshot) != digest &&
                document_snapshot_digest(snapshot) != snapshot_digest,
                "both document digest surfaces must bind every transform field");
    }
    for (const auto& key : {"version", "boundary_id", "pivot", "rotation_radians", "flip_horizontal", "flip_vertical", "offset"}) {
        auto missing = wire; missing.erase(key);
        bool rejected = false;
        try { (void)decode_boundary_transform(missing); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "transform decoder must require every field");
    }
}

void test_legacy_constraint_proof_digest_vectors() {
    sketch::test::DetachedDocumentSnapshotFixture snapshot(Document::create().snapshot());
    const_cast<std::string&>(snapshot.document_id()) = "legacy-constraint-digest-vectors";
    auto& proof = snapshot.history().front().boundary_constraint_changes;
    const BoundaryGeometryEdit boundary{"legacy-area", BoundaryGeometryEditKind::move_vertex,
        "legacy-vertex", {3, 1}};
    const ConstraintWallGeometryEdit straight{"legacy-wall", {{0,0},{2,0},0}, std::nullopt, 1};
    const ConstraintWallGeometryEdit curve{"legacy-wall", {{0,0},{2,0},0.4}, std::nullopt, 2};
    const ConstraintWallGeometryEdit length{"legacy-wall", {{0,0},{2,0},std::numbers::pi},
        parse_quantity("3.141592653589793 m"), 3};
    const std::vector<ApplyBoundaryConstraintChanges> legacy{
        {0, {boundary}, {}, "Legacy proof", {}},
        {0, {boundary}, {}, "Legacy proof", {straight}},
        {0, {boundary}, {}, "Legacy proof", {curve}},
        {0, {}, {}, "Legacy proof", {straight}},
        {0, {}, {}, "Legacy proof", {length}}};
    // Frozen from the pre-v6 ordered snapshot encoding, independently hashed.
    const char* full[] {
        "37e1cdd1743af1876f51de8d348e6f0b8b100b70469ee8ee972e31478cef53a6",
        "adf932d0d4a4639948c97d62fc51e5893c2ec5d9e9ce277190174aad029e502a",
        "53152ff72981e288727d11bf2d6f9a3dde67f4116494e92b2f72b0cf870158f0",
        "a416cbeb45283b4e47332d1a3c41a3438418820024cc790555ba65d65ca505a7",
        "77de74ba0c74006301da02a90ad368d2c090e7aed9eb4be42a11b443f7dd1c5f"};
    const char* authoring[] {
        "f13337e22d1865f4fb6f68b3c05eef4640f7e59d305c1be3ea0f87f9aeaa8639",
        "33d00597509a6835d983dee17416855667e90cccde59562895a30d1c2022e46f",
        "ce0364222e6194eec1195b145a99fc79285360589fbe4ff6e3dd2434938e9c72",
        "b7234ee77dc117fc8cb8324b79339f8de3f62b061234930237a34286d0406ffb",
        "e232ecb902140a19ea17db35207a6afde3f530ce18904b3bf0ca005119a1e47a"};
    for (std::size_t i = 0; i < legacy.size(); ++i) {
        proof = legacy[i];
        const auto wire = command_to_json(*proof);
        require(wire.at("version") == i + 1 && !wire.contains("physical_entity_changes") &&
            !wire.contains("exterior_source_edits"), "v1-v5 proofs must retain their original envelope shape");
        require(command_to_json(command_from_json(wire)) == wire,
            "legacy constraint proof must retain its exact command codec roundtrip");
        require(document_snapshot_digest(snapshot) == full[i] &&
            document_authoring_source_digest_v1(snapshot) == authoring[i],
            "v6 support must preserve every legacy constraint digest vector");
    }
}

void test_live_source_proof_digest_binding() {
    sketch::test::DetachedDocumentSnapshotFixture snapshot(Document::create().snapshot());
    auto& proof = snapshot.history().front().boundary_constraint_changes;
    ApplyBoundaryConstraintChanges original{0, {}, {}, "Live source proof"};
    original.physical_entity_changes.push_back(EntityChange::upsert(
        {"source-wall", "wall", {{"baseline", {{"start", {0,0}}, {"end", {4,0}}, {"sweep_radians", 0.0}}},
            {"thickness_m", 0.4}, {"height_m", 3.0}, {"elevation_m", 0.0}}, false, {{"vendor", "retain"}}}));
    BoundaryGeometryEdit exterior;
    exterior.boundary_id = exterior.target_id = "source-area";
    exterior.kind = BoundaryGeometryEditKind::redefine_boundary;
    exterior.replacement_wall_source_ids = {"source-wall", "source-right", "source-top", "source-left"};
    exterior.replacement_segments = nlohmann::json::array();
    const Vec2 corners[]{{0,0},{4,0},{4,3},{0,3}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto a = corners[i], b = corners[(i + 1) % 4];
        exterior.replacement_segments.push_back({{"segment_id", "source-edge-" + std::to_string(i)},
            {"start_vertex_id", "source-corner-" + std::to_string(i)},
            {"end_vertex_id", "source-corner-" + std::to_string((i + 1) % 4)},
            {"start", {a.x,a.y}}, {"end", {b.x,b.y}}, {"sweep_radians", 0.0}});
    }
    original.exterior_source_edits.push_back(exterior);
    original.exterior_source_completion = true;
    proof = original;
    const auto full = document_snapshot_digest(snapshot);
    const auto authoring = document_authoring_source_digest_v1(snapshot);
    const auto wire = command_to_json(original);
    require(wire.at("version") == 6 && !wire.contains("supplemental_entity_changes") &&
        !wire.contains("supplemental_asset_changes") && command_to_json(command_from_json(wire)) == wire,
        "source proof must retain both new lanes through the command codec");
    const std::vector<std::function<void(ApplyBoundaryConstraintChanges&)>> mutations{
        [](auto& p) { p.physical_entity_changes.front().entity.properties["thickness_m"] = 0.6; },
        [](auto& p) { p.physical_entity_changes.front().entity.extensions["vendor"] = "changed"; },
        [](auto& p) { p.physical_entity_changes.front().entity.id = "other-wall"; },
        [](auto& p) { p.exterior_source_edits.front().replacement_segments[0]["start"][0] = -1.0; },
        [](auto& p) { p.exterior_source_edits.front().boundary_id = p.exterior_source_edits.front().target_id = "other-area"; },
        [](auto& p) { p.exterior_source_edits.front().replacement_wall_source_ids.front() = "other-wall"; },
        [](auto& p) { p.physical_entity_changes.clear(); }};
    for (const auto& mutate : mutations) {
        proof = original;
        mutate(*proof);
        require(document_snapshot_digest(snapshot) != full &&
            document_authoring_source_digest_v1(snapshot) != authoring,
            "both digest surfaces must bind source completion geometry, identity and opaque physical data");
    }
    for (const auto* key : {"physical_entity_changes", "exterior_source_edits", "wall_edits"}) {
        auto missing = wire;
        missing.erase(key);
        bool rejected = false;
        try { (void)command_from_json(missing); } catch (const std::exception&) { rejected = true; }
        require(rejected, "v6 decoder must require every lane rather than silently omitting proof");
    }
    for (const int old_version : {1,2,3,4,5}) {
        auto mislabeled = wire;
        mislabeled["version"] = old_version;
        bool rejected = false;
        try { (void)command_from_json(mislabeled); } catch (const std::exception&) { rejected = true; }
        require(rejected, "old command envelopes must refuse the new source proof lanes");
    }

    original.supplemental_source_completion = true;
    original.supplemental_entity_changes = {
        EntityChange::upsert({"source-label", "label", {{"text", "Measured"}}, false, {{"vendor", "retain"}}}),
        EntityChange::erase("removed-label")};
    original.supplemental_asset_changes = {
        AssetChange::upsert(Asset::create("source-image", "application/octet-stream",
            {std::byte{1}, std::byte{2}}, {{"caption", "Measured"}})),
        AssetChange::erase("removed-image")};
    proof = original;
    const auto mixed_wire = command_to_json(*proof);
    const auto mixed_full = document_snapshot_digest(snapshot);
    const auto mixed_authoring = document_authoring_source_digest_v1(snapshot);
    require(mixed_wire.at("version") == 7 && command_to_json(command_from_json(mixed_wire)) == mixed_wire,
        "v7 must roundtrip exact supplemental entity and asset intents");
    const std::vector<std::function<void(ApplyBoundaryConstraintChanges&)>> mixed_mutations{
        [](auto& p) { p.supplemental_entity_changes[0].entity.properties["text"] = "Changed"; },
        [](auto& p) { p.supplemental_entity_changes[0].entity.id = "changed-label"; },
        [](auto& p) { p.supplemental_entity_changes[0].entity.type = "symbol"; },
        [](auto& p) { p.supplemental_entity_changes[0].entity.required = true; },
        [](auto& p) { p.supplemental_entity_changes[0].entity.extensions["vendor"] = "Changed"; },
        [](auto& p) { p.supplemental_entity_changes[1].entity_id = "other-removed-label"; },
        [](auto& p) { p.supplemental_entity_changes[0] = EntityChange::erase("source-label"); },
        [](auto& p) { std::swap(p.supplemental_entity_changes[0], p.supplemental_entity_changes[1]); },
        [](auto& p) { p.supplemental_entity_changes.clear(); },
        [](auto& p) { p.supplemental_asset_changes[0].asset = Asset::create("source-image", "application/octet-stream",
            {std::byte{3}, std::byte{2}}, {{"caption", "Measured"}}); },
        [](auto& p) { p.supplemental_asset_changes[0].asset.id = "changed-image"; },
        [](auto& p) { p.supplemental_asset_changes[0].asset.media_type = "image/png"; },
        [](auto& p) { p.supplemental_asset_changes[0].asset.metadata["caption"] = "Changed"; },
        [](auto& p) { p.supplemental_asset_changes[1].asset_id = "other-removed-image"; },
        [](auto& p) { p.supplemental_asset_changes[0] = AssetChange::erase("source-image"); },
        [](auto& p) { std::swap(p.supplemental_asset_changes[0], p.supplemental_asset_changes[1]); },
        [](auto& p) { p.supplemental_asset_changes.clear(); }};
    for (const auto& mutate : mixed_mutations) {
        proof = original;
        mutate(*proof);
        require(document_snapshot_digest(snapshot) != mixed_full &&
            document_authoring_source_digest_v1(snapshot) != mixed_authoring,
            "both digest surfaces must bind every supplemental entity and asset intent field");
    }
    for (const auto* key : {"supplemental_entity_changes", "supplemental_asset_changes"}) {
        auto missing = mixed_wire;
        missing.erase(key);
        bool rejected = false;
        try { (void)command_from_json(missing); } catch (const std::exception&) { rejected = true; }
        require(rejected, "v7 decoder must require every supplemental lane");
    }
    auto mislabeled = mixed_wire;
    mislabeled["version"] = 6;
    bool rejected = false;
    try { (void)command_from_json(mislabeled); } catch (const std::exception&) { rejected = true; }
    require(rejected, "v6 must reject supplemental intents rather than silently downgrade them");
    auto stripped = original;
    stripped.supplemental_entity_changes.clear();
    stripped.supplemental_asset_changes.clear();
    const auto stripped_wire = command_to_json(stripped);
    require(stripped_wire.at("version") == 7 && command_to_json(command_from_json(stripped_wire)) == stripped_wire,
        "v7 discriminator must survive decode and re-encode even when supplemental vectors are stripped");
}
} // namespace

int main() {
    try {
        test_shared_immutable_snapshot_authority_and_lifetime();
        test_full_snapshot_binding();
        test_candidate_maps_preserve_identity_authority();
        test_authoring_source_survives_save_bookkeeping();
        test_digest_format_vectors();
        test_historical_authoring_bindings();
        test_translation_proof_digest_and_codec();
        test_transform_proof_digest_and_codec();
        test_legacy_constraint_proof_digest_vectors();
        test_live_source_proof_digest_binding();
        std::cout << "Document digest tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "document_digest_tests: " << error.what() << '\n';
        return 1;
    }
}
