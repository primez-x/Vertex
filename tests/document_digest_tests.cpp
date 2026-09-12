#include "sketch/document_digest.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
        [](auto& record) { record.boundary_translation = BoundaryTranslation{"boundary-1", {8, -4}}; },
    };
    for (const auto& mutate : mutations) {
        auto changed = source;
        mutate(const_cast<std::vector<RevisionRecord>&>(changed.history()).front());
        require(changed.entities() == source.entities() && changed.revision() == source.revision(),
                "history fixture must leave the visible head and revision unchanged");
        require(document_snapshot_digest(changed) != original_digest,
                "snapshot digest missed changed history, navigation or actual asset data");
        require(document_authoring_source_digest_v1(changed) != original_authoring_digest,
                "authoring source missed changed history, navigation or actual asset data");
    }
    require(document_snapshot_digest(source) == original_digest,
            "copied snapshot mutation changed the original snapshot");
    auto mismatched_asset = source;
    auto& asset_map = const_cast<std::vector<RevisionRecord>&>(mismatched_asset.history()).front().assets;
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
    auto forged = current;
    const_cast<std::vector<RevisionRecord>&>(forged.history()).front().action = "forged";
    require(document_authoring_source_digest_v1_at_revision(forged, 0) !=
                document_authoring_source_digest_v1(baselines.front()),
            "changed retained baseline data must alter its binding");
}

void test_translation_proof_digest_and_codec() {
    auto snapshot = Document::create().snapshot();
    auto& proof = const_cast<std::vector<RevisionRecord>&>(snapshot.history()).front().boundary_translation;
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
    auto snapshot = Document::create().snapshot();
    auto& proof = const_cast<std::vector<RevisionRecord>&>(snapshot.history()).front().boundary_transform;
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
} // namespace

int main() {
    try {
        test_full_snapshot_binding();
        test_candidate_maps_preserve_identity_authority();
        test_authoring_source_survives_save_bookkeeping();
        test_digest_format_vectors();
        test_historical_authoring_bindings();
        test_translation_proof_digest_and_codec();
        test_transform_proof_digest_and_codec();
        std::cout << "Document digest tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "document_digest_tests: " << error.what() << '\n';
        return 1;
    }
}
