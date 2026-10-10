#include "sketch/mixed_clipboard_placement.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/terrain_surface.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t change_limit = 4096;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Mixed clipboard placement: ") + reason);
}
void identity(std::string_view id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid identity");
}
bool exact(const Json& a, const Json& b) {
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (const auto& [key, child] : a.items()) {
            const auto found = b.find(key);
            if (found == b.end() || !exact(child, *found)) return false;
        }
        return true;
    }
    if (a.is_array()) {
        for (std::size_t i = 0; i < a.size(); ++i) if (!exact(a[i], b[i])) return false;
        return true;
    }
    return a == b && (!a.is_number_float() || std::signbit(a.get<double>()) == std::signbit(b.get<double>()));
}
bool exact(const Entity& a, const Entity& b) {
    return a.id == b.id && a.type == b.type && a.required == b.required &&
        exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
bool exact(const CornerWindowTransfer& a, const CornerWindowTransfer& b) {
    if (!exact(a.owner, b.owner) || a.dimensions.size() != b.dimensions.size()) return false;
    for (std::size_t i = 0; i < 2; ++i)
        if (!exact(a.walls[i], b.walls[i]) || !exact(a.cuts[i], b.cuts[i])) return false;
    for (std::size_t i = 0; i < a.dimensions.size(); ++i)
        if (!exact(a.dimensions[i], b.dimensions[i])) return false;
    return true;
}

// Bound raw prepared output before allocating copies, rather than serializing
// an untrusted replacement graph merely to discover its size.
struct PreparedBudget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t amount) {
        if (amount > 4 * 1024 * 1024 - bytes) reject("prepared payload byte budget exceeded");
        bytes += amount;
    }
    void text(std::string_view value) { add(2); for (unsigned char c : value) add(c < 0x20 ? 6 : c == '"' || c == '\\' ? 2 : 1); }
    void read(const Json& value, unsigned depth = 0) {
        if (depth > 64 || ++nodes > 100000) reject("prepared payload complexity budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable prepared payload");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_object()) {
            add(2 + value.size());
            for (const auto& [key, child] : value.items()) { text(key); add(1); read(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + value.size()); for (const auto& child : value) read(child, depth + 1);
        } else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
    void entity(const Entity& value) {
        identity(value.id); identity(value.type);
        if (!value.properties.is_object() || !value.extensions.is_object()) reject("invalid prepared envelope");
        for (const auto* key : {"id", "type", "properties", "required"})
            if (value.extensions.contains(key)) reject("prepared extensions shadow an envelope field");
        add(80); text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
};

struct Reservation {
    const Ids& fresh;
    std::size_t nodes{}, bytes{};
    static constexpr std::size_t max_nodes = 4 * 1024 * 1024, max_bytes = 64 * 1024 * 1024;
    void text(std::string_view value) {
        if (value.size() > max_bytes - bytes) reject("retained identity byte budget exceeded");
        bytes += value.size();
        if (fresh.contains(value)) reject("fresh identity occurs in passive source or retained history");
    }
    void read(const Json& value, unsigned depth = 0) {
        if (depth > 64 || ++nodes > max_nodes) reject("retained identity complexity budget exceeded");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
    void entity(const Entity& value) {
        if (++nodes > max_nodes) reject("retained entity budget exceeded");
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
    void changes(const std::vector<EntityChange>& values) {
        if (values.size() > max_nodes - nodes) reject("retained change budget exceeded");
        for (const auto& value : values) {
            ++nodes;
            if (value.kind == EntityChangeKind::upsert) entity(value.entity); else text(value.entity_id);
        }
    }
};

void reserve(const DocumentSnapshot& destination, const MixedClipboardTransfer& passive, const Ids& fresh) {
    const auto& history = destination.history();
    if (history.empty() || history.size() > 4096 || destination.revision() >= history.size())
        reject("retained history exceeds admission bounds");
    Reservation scan{fresh};
    // The closed passive admission has already bounded this traversal.
    if (passive.ordinary) scan.read(*passive.ordinary);
    for (const auto& corner : passive.corners) {
        scan.entity(corner.owner);
        for (const auto& wall : corner.walls) scan.entity(wall);
        for (const auto& cut : corner.cuts) scan.entity(cut);
        for (const auto& dimension : corner.dimensions) scan.entity(dimension);
    }
    for (const auto& catalog : passive.catalogs) scan.entity(catalog);
    for (const auto& sky : passive.skylights) { scan.entity(sky.roof); scan.text(sky.opening_id); }
    scan.text(destination.document_id());
    for (const auto& [name, revision] : destination.named_revisions()) { (void)revision; scan.text(name); }
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& record = history[index];
        if (record.revision != index) reject("noncontiguous retained history");
        scan.text(record.action); if (record.name) scan.text(*record.name);
        for (const auto& [id, entity] : record.entities) { scan.text(id); scan.entity(entity); }
        for (const auto& [id, asset] : record.assets) {
            if (++scan.nodes > Reservation::max_nodes) reject("retained asset budget exceeded");
            scan.text(id); scan.text(asset.id); scan.text(asset.media_type); scan.text(asset.sha256); scan.read(asset.metadata);
        }
        if (record.boundary_translation) scan.text(record.boundary_translation->boundary_id);
        if (record.boundary_transform) scan.text(record.boundary_transform->boundary_id);
        if (record.boundary_geometry_edit) scan.read(encode_boundary_geometry_edit(*record.boundary_geometry_edit));
        if (record.boundary_translations) {
            scan.changes(record.boundary_translations->entity_changes);
            for (const auto& row : record.boundary_translations->translations) scan.text(row.boundary_id);
        }
        if (record.boundary_transforms) {
            scan.changes(record.boundary_transforms->entity_changes);
            for (const auto& row : record.boundary_transforms->transformations) scan.text(row.boundary_id);
            for (const auto& row : record.boundary_transforms->source_transformations) scan.text(row.owner_id);
        }
        if (record.boundary_constraint_changes) {
            const auto& proof = *record.boundary_constraint_changes;
            scan.changes(proof.entity_changes); scan.changes(proof.physical_entity_changes);
            scan.changes(proof.supplemental_entity_changes); scan.changes(proof.selection_entity_changes);
            if (proof.rigid_group_transform) scan.changes(proof.rigid_group_transform->entity_changes);
            for (const auto* raw : {&proof.room_review_intent, &proof.room_review_geometry_proof,
                &proof.phase_room_review_intent, &proof.phase_constraint_authoring_intent,
                &proof.independent_drawing_removal_intent, &proof.mixed_selection_removal_intent,
                &proof.ordinary_selection_removal_intent, &proof.phase_selection_removal_intent}) scan.read(*raw);
            if (proof.room_review_additional_intents.size() > Reservation::max_nodes - scan.nodes)
                reject("retained intent budget exceeded");
            for (const auto& raw : proof.room_review_additional_intents) scan.read(raw);
            // Canonical retained proof serialization covers typed retired IDs,
            // quantity receipts and future additive proof lanes too. This is
            // identity inspection of an actual retained command, not replay or
            // a manufactured source. Historical inline asset proofs retain
            // their conservative encoded-string reservation as well.
            scan.read(command_to_json(Command{proof}));
        }
        if (record.phase_entity_import) {
            for (const auto& id : record.phase_entity_import->entity_ids) scan.text(id);
            for (const auto& id : record.phase_entity_import->asset_ids) scan.text(id);
        }
    }
}

Entity ordinary_entity(const Json& value) {
    return {value.at("id").get<std::string>(), value.at("type").get<std::string>(),
        value.at("properties"), value.at("required").get<bool>(), value.at("extensions")};
}
const Json& opening_row(const Entity& roof, const std::string& id) {
    const Json* result = nullptr;
    for (const auto& row : roof.properties.at("roof_openings")) if (row.at("id") == id) {
        if (result) reject("duplicate mapped roof child");
        result = &row;
    }
    if (!result) reject("mapped roof child is missing");
    return *result;
}
void merge_materials(Entity& target, const Entity& pool, bool fresh) {
    if (target.type != "assembly_model") reject("material mapping does not name a catalog");
    auto& model = target.properties.at("model");
    const auto& projection = pool.properties.at("model");
    // v7 is the codec's closed union of retained row envelopes. It can carry
    // both older uncolored rows and newer appearance rows without rewriting
    // either row or imposing a newer global type/placement shape.
    if (fresh && model.at("schema") != projection.at("schema")) model.at("schema") = "sketch.assemblies.v7";
    // Source admission permits compatible material-only projections with
    // lane-local carrier context. Preserve the chosen destination carrier.
    for (const auto& [key, value] : projection.items()) {
        if (key == "schema" || key == "materials" || key == "types" || key == "instances") continue;
        if (!model.contains(key) || !exact(model.at(key), value)) reject("shared catalog model metadata conflicts");
    }
    for (const auto& [key, value] : model.items()) {
        (void)value;
        if (key != "schema" && key != "materials" && key != "types" && key != "instances" && !projection.contains(key))
            reject("shared catalog model metadata conflicts");
    }
    for (const auto& material : projection.at("materials")) {
        Json* found = nullptr;
        for (auto& row : model.at("materials")) if (row.at("id") == material.at("id")) {
            if (found) reject("duplicate destination material definition");
            found = &row;
        }
        if (found) { if (!exact(*found, material)) reject("shared material definitions conflict"); }
        else {
            if (!fresh) reject("reused actual catalog does not contain the complete mixed material pool");
            model.at("materials").push_back(material);
        }
    }
    (void)AssemblyModel::from_json(model);
}
} // namespace

MixedClipboardPlacement prepare_mixed_clipboard_placement(const DocumentSnapshot& destination,
    const MixedClipboardPlacementRequest& request, Revision expected_revision) {
    if (!destination.is_editable() || destination.revision() != expected_revision) reject("read-only or stale destination");
    validate_mixed_clipboard_transfer(request.transfer);
    if (request.corners.size() != request.transfer.corners.size() || request.skylights.size() > 1000 ||
        request.imported_material_catalogs.size() > 128 || request.ordinary.entity_changes.size() > change_limit ||
        request.identity_mapping.size() > 100000 || request.material_catalog_mapping.size() > 256 ||
        request.roof_opening_identity_mapping.size() > 100000)
        reject("prepared family roster exceeds admission bounds");
    if (!request.ordinary.asset_changes.empty()) reject("ordinary asset changes are not a passive clipboard lane");
    if (request.transfer.ordinary && request.ordinary.expected_revision != expected_revision)
        reject("ordinary preparation binds another destination revision");
    if (!request.transfer.ordinary && (!request.ordinary.entity_changes.empty() || !request.identity_mapping.empty() ||
        !request.roof_opening_identity_mapping.empty())) reject("ordinary preparation has no transported graph");

    PreparedBudget input_budget;
    const auto mapping_budget = [&](const std::string& source, const std::string& target) {
        identity(source); identity(target);
        if (input_budget.nodes > 100000 - 2) reject("prepared mapping complexity budget exceeded");
        input_budget.nodes += 2; input_budget.text(source); input_budget.text(target);
    };
    for (const auto& [source, target] : request.identity_mapping) mapping_budget(source, target);
    for (const auto& [source, target] : request.material_catalog_mapping) mapping_budget(source, target);
    for (const auto& [source, target] : request.roof_opening_identity_mapping) {
        mapping_budget(source.first, target); mapping_budget(source.second, target);
    }
    Entities additions, originals;
    for (const auto& change : request.ordinary.entity_changes) {
        if (change.kind != EntityChangeKind::upsert) reject("ordinary preparation may contain fresh additions only");
        input_budget.entity(change.entity);
        if (destination.entities().contains(change.entity.id)) reject("ordinary preparation would overwrite actual content");
        if (!additions.emplace(change.entity.id, change.entity).second) reject("duplicate ordinary addition");
    }
    for (const auto& catalog : request.imported_material_catalogs) {
        input_budget.entity(catalog);
        if (catalog.type != "assembly_model" || destination.entities().contains(catalog.id) ||
            !additions.emplace(catalog.id, catalog).second) reject("imported catalog requires one fresh addition");
        const auto model = AssemblyModel::from_json(catalog.properties.at("model"));
        if (!model.types().empty() || !model.instances().empty()) reject("corner catalog imports must be material-only");
    }
    if (request.transfer.ordinary) for (const auto& row : request.transfer.ordinary->at("entities")) {
        auto original = ordinary_entity(row);
        originals.emplace(original.id, std::move(original));
    }

    MixedClipboardPlacement result;
    result.identity_mapping = request.identity_mapping;
    result.material_catalog_mapping = request.material_catalog_mapping;
    result.roof_opening_identity_mapping = request.roof_opening_identity_mapping;
    Ids fresh, ordinary_targets;
    const auto fresh_identity = [&](const std::string& id) {
        identity(id);
        if (destination.entities().contains(id) || !fresh.insert(id).second) reject("fresh identities overlap another member or actual content");
    };
    for (const auto& [source_id, target_id] : request.identity_mapping) {
        identity(source_id); identity(target_id);
        if (!ordinary_targets.insert(target_id).second) reject("ordinary identity map is not injective");
        const auto original = originals.find(source_id);
        const auto actual = destination.entities().find(target_id);
        const bool reused = original != originals.end() && original->second.type == "assembly_model" &&
            source_id == target_id && actual != destination.entities().end() && actual->second.type == "assembly_model";
        if (!reused) fresh_identity(target_id);
    }
    for (const auto& [id, original] : originals) {
        const auto mapped = request.identity_mapping.find(id);
        if (mapped == request.identity_mapping.end()) reject("ordinary mapping omits a transported entity");
        const auto target = additions.find(mapped->second);
        if (target == additions.end()) {
            if (original.type != "assembly_model" || mapped->second != id || !destination.entities().contains(id))
                reject("ordinary mapped entity has no prepared addition");
        } else if (target->second.type != original.type || target->second.required != original.required)
            reject("ordinary mapped entity changed its envelope role");
    }
    Ids ordinary_owners;
    for (const auto& [id, original] : originals) { (void)original; ordinary_owners.insert(request.identity_mapping.at(id)); }
    Ids imported_catalogs;
    for (const auto& catalog : request.imported_material_catalogs) imported_catalogs.insert(catalog.id);
    for (const auto& change : request.ordinary.entity_changes)
        if (!ordinary_owners.contains(change.entity.id)) reject("ordinary addition has no transported source owner");
    for (const auto& [id, entity] : additions) {
        (void)entity;
        if (!fresh.contains(id)) fresh_identity(id);
    }
    for (const auto& [source, target] : request.roof_opening_identity_mapping) {
        const auto roof = originals.find(source.first);
        if (roof == originals.end() || roof->second.type != "roof") reject("qualified child mapping has no transported roof");
        (void)opening_row(roof->second, source.second);
        const auto& fresh_roof = additions.at(request.identity_mapping.at(source.first));
        auto expected = opening_row(roof->second, source.second);
        expected.at("id") = target;
        if (!exact(expected, opening_row(fresh_roof, target))) reject("transported roof child differs from its exact mapped source row");
        fresh_identity(target);
    }
    Entities fresh_roofs;
    for (const auto& [id, roof] : originals) if (roof.type == "roof") {
        const auto& target = additions.at(request.identity_mapping.at(id));
        // Only exact already prepared ordinary additions are offered as fresh
        // hosts. Caller choices cannot supply or substitute another roof.
        fresh_roofs.emplace(target.id, target);
        if (!roof.properties.contains("roof_openings")) {
            if (target.properties.contains("roof_openings")) reject("ordinary roof preparation introduces a child roster");
            continue;
        }
        if (target.properties.at("roof_openings").size() != roof.properties.at("roof_openings").size())
            reject("ordinary roof preparation drops or introduces children");
        for (const auto& row : roof.properties.at("roof_openings"))
            if (!request.roof_opening_identity_mapping.contains({id, row.at("id").get<std::string>()}))
                reject("ordinary roof preparation lacks a fresh identity for every child");
    }

    Ids catalogs;
    for (const auto& [id, original] : originals) if (original.type == "assembly_model") catalogs.insert(id);
    for (const auto& catalog : request.transfer.catalogs) catalogs.insert(catalog.id);
    if (request.material_catalog_mapping.size() != catalogs.size()) reject("material map must cover exactly the transported catalogs");
    Ids material_targets;
    for (const auto& id : catalogs) {
        const auto mapped = request.material_catalog_mapping.find(id);
        if (mapped == request.material_catalog_mapping.end()) reject("transported catalog lacks a destination mapping");
        identity(mapped->second);
        if (!material_targets.insert(mapped->second).second) reject("distinct source catalogs may not collapse identities");
        if (originals.contains(id) && request.identity_mapping.at(id) != mapped->second)
            reject("ordinary and corner material mappings disagree");
        const auto prepared = additions.find(mapped->second);
        const auto actual = destination.entities().find(mapped->second);
        if (prepared == additions.end() && (actual == destination.entities().end() || actual->second.type != "assembly_model"))
            reject("mapped catalog carrier is missing");
        if (prepared != additions.end() && prepared->second.type != "assembly_model") reject("mapped catalog has another role");
        if (prepared != additions.end() && originals.contains(id)) {
            auto retained = originals.at(id); retained.id = mapped->second;
            if (!exact(retained, prepared->second)) reject("ordinary catalog must retain its raw source carrier before reconciliation");
        }
    }
    for (const auto& id : imported_catalogs)
        if (!material_targets.contains(id)) reject("fresh catalog import has no transported material source");
    for (const auto& imported : request.imported_material_catalogs) {
        const auto pool = std::find_if(request.transfer.catalogs.begin(), request.transfer.catalogs.end(),
            [&](const auto& source) { return request.material_catalog_mapping.at(source.id) == imported.id; });
        if (pool == request.transfer.catalogs.end()) reject("corner catalog import has no exact passive projection");
        auto retained = *pool; retained.id = imported.id;
        if (!exact(retained, imported)) reject("corner catalog import changes passive carrier content");
    }
    for (const auto& catalog : request.transfer.catalogs) {
        const auto& mapped = request.material_catalog_mapping.at(catalog.id);
        const auto prepared = additions.find(mapped);
        if (prepared != additions.end()) merge_materials(prepared->second, catalog, true);
        else { auto carrier = destination.entities().at(mapped); merge_materials(carrier, catalog, false); }
    }

    std::vector<CornerWindowCloneRequest> corners;
    std::vector<Entity> fresh_hosts;
    Ids consumed_corners, host_ids;
    const auto mapping = [&](const std::string& source, const std::string& target) {
        const auto [entry, inserted] = result.identity_mapping.emplace(source, target);
        if (!inserted && entry->second != target) reject("mixed member mappings conflict");
    };
    for (const auto& choice : request.corners) {
        const auto found = std::find_if(request.transfer.corners.begin(), request.transfer.corners.end(),
            [&](const auto& corner) { return corner.owner.id == choice.transfer.owner.id; });
        if (found == request.transfer.corners.end() || !exact(*found, choice.transfer) ||
            !consumed_corners.insert(found->owner.id).second) reject("corner placement does not cover exact passive members");
        fresh_identity(choice.owner_id); mapping(found->owner.id, choice.owner_id);
        for (std::size_t leg = 0; leg < 2; ++leg) {
            fresh_identity(choice.opening_ids[leg]); mapping(found->cuts[leg].id, choice.opening_ids[leg]);
            const auto selected = originals.find(found->walls[leg].id);
            if (selected != originals.end()) {
                const auto& mapped = request.identity_mapping.at(selected->first);
                if (selected->second.type != "wall" || !exact(selected->second, found->walls[leg]) || choice.wall_ids[leg] != mapped)
                    reject("selected wall host must dominate its transported corner leg");
                if (host_ids.insert(mapped).second) fresh_hosts.push_back(additions.at(mapped));
            } else if (!destination.entities().contains(choice.wall_ids[leg]))
                reject("unselected corner host must bind an actual destination wall");
        }
        if (choice.dimension_ids.size() != found->dimensions.size()) reject("corner dimension mapping is incomplete");
        for (const auto& dimension : found->dimensions) {
            const auto mapped = choice.dimension_ids.find(dimension.id);
            if (mapped == choice.dimension_ids.end()) reject("corner dimension lacks a fresh mapping");
            fresh_identity(mapped->second); mapping(dimension.id, mapped->second);
        }
        corners.push_back(choice);
    }

    using Child = std::pair<std::string, std::string>;
    std::map<Child, const RoofOpeningCloneSource*> skylights;
    for (const auto& sky : request.transfer.skylights) {
        const Child key{sky.roof.id, sky.opening_id};
        skylights.emplace(key, &sky);
        if (originals.contains(sky.roof.id)) {
            if (!exact(originals.at(sky.roof.id), sky.roof)) reject("selected skylight roof conflicts with ordinary source");
            const auto mapped = request.roof_opening_identity_mapping.find(key);
            if (mapped == request.roof_opening_identity_mapping.end()) reject("selected roof does not carry its transported skylight");
        }
    }
    std::set<Child> consumed_skylights;
    std::vector<RoofOpeningGroupClonePlacement> placements;
    for (const auto& choice : request.skylights) {
        if (choice.clones.empty() || choice.clones.size() > 1000) reject("invalid skylight placement group size");
        RoofOpeningGroupClonePlacement prepared{{}, choice.destination_roof_id, choice.destination_anchor_world};
        for (const auto& clone : choice.clones) {
            const Child key{clone.source.roof.id, clone.source.opening_id};
            const auto captured = skylights.find(key);
            if (captured == skylights.end() || !exact(captured->second->roof, clone.source.roof) ||
                !consumed_skylights.insert(key).second) reject("skylight choices do not cover exact passive members");
            const auto dominated_mapping = request.roof_opening_identity_mapping.find(key);
            if (dominated_mapping != request.roof_opening_identity_mapping.end()) {
                if (clone.opening_id != dominated_mapping->second ||
                    choice.destination_roof_id != request.identity_mapping.at(key.first))
                    reject("selected roof must dominate its child placement");
                continue;
            }
            fresh_identity(clone.opening_id);
            if (!result.roof_opening_identity_mapping.emplace(key, clone.opening_id).second)
                reject("duplicate skylight mapping");
            prepared.clones.push_back(clone);
        }
        if (!prepared.clones.empty()) {
            if (!destination.entities().contains(prepared.destination_roof_id) &&
                !fresh_roofs.contains(prepared.destination_roof_id))
                reject("independent skylight destination must be an actual or mapped prepared ordinary roof host");
            placements.push_back(std::move(prepared));
        }
    }
    for (const auto& [key, source] : skylights) {
        (void)source;
        const auto carried = request.roof_opening_identity_mapping.find(key);
        if (!consumed_skylights.contains(key) && carried == request.roof_opening_identity_mapping.end())
            reject("an independent transported skylight has no destination choice");
    }
    // All ordinary, nested, material, corner and skylight names enter the same
    // reservation before either family producer receives fresh identities.
    reserve(destination, request.transfer, fresh);
    if (!corners.empty()) {
        auto cloned = corner_window_group_clone_command(destination, corners, expected_revision, {}, fresh_hosts);
        for (auto& change : cloned.entity_changes) {
            change.entity = remap_architectural_material_source_refs(change.entity, request.material_catalog_mapping);
            if (!additions.emplace(change.entity.id, std::move(change.entity)).second) reject("corner overlaps another fresh addition");
        }
    }
    std::map<std::string, RoofEditIntent, std::less<>> roof_edits;
    for (const auto& placement : placements) {
        auto edit = prepare_roof_opening_group_clone_placement(destination.entities(), placement, fresh_roofs);
        const auto [entry, inserted] = roof_edits.try_emplace(edit.roof_id, edit);
        if (!inserted) {
            auto& combined = *entry->second.openings;
            const auto& addition = *edit.openings;
            combined.uses_skylight_schema = combined.uses_skylight_schema || addition.uses_skylight_schema;
            combined.uses_clone_schema = combined.uses_clone_schema || addition.uses_clone_schema;
            combined.uses_rotation_schema = combined.uses_rotation_schema || addition.uses_rotation_schema;
            for (auto& row : edit.openings->upserts) combined.upserts.push_back(std::move(row));
        }
    }
    for (auto& [id, edit] : roof_edits) {
        const auto fresh_host = fresh_roofs.find(id);
        if (fresh_host != fresh_roofs.end()) {
            // Replay the complete combined roster against the one original
            // prepared host. Never validate against a previous group's output,
            // and never export original-map edit authority for this fresh ID.
            additions.at(id) = replay_roof_edit_entity(fresh_host->second, edit);
        } else result.roof_intents.push_back(std::move(edit));
    }
    if (!result.roof_intents.empty()) {
        const auto replayed = replay_roof_edit_entities(destination.entities(), result.roof_intents);
        for (const auto& [id, entity] : replayed)
            if (!exact(entity, destination.entities().at(id))) result.roof_candidates.emplace(id, entity);
    }
    if (additions.size() + result.roof_candidates.size() > change_limit) reject("combined change row budget exceeded");
    PreparedBudget output_budget;
    for (auto& [id, entity] : additions) {
        (void)id;
        output_budget.entity(entity);
        result.fresh_entity_changes.push_back(EntityChange::upsert(std::move(entity)));
    }
    for (const auto& [id, entity] : result.roof_candidates) { (void)id; output_budget.entity(entity); }
    for (const auto& intent : result.roof_intents) output_budget.read(encode_roof_edit_intent(intent));
    return result;
}
ApplyEntityChanges translated_mixed_clipboard_ordinary_graph(const DocumentSnapshot& source,
    const ApplyEntityChanges& prepared,const std::map<std::string,Vec2,std::less<>>& owner_offsets,
    const std::map<std::pair<std::string,std::string>,Vec2>& annotation_offsets) {
    if (!source.is_editable() || prepared.expected_revision!=source.revision() ||
        prepared.entity_changes.empty() || prepared.entity_changes.size()>change_limit ||
        !prepared.asset_changes.empty() || owner_offsets.size()!=prepared.entity_changes.size())
        reject("fresh graph translation requires one captured complete fresh inventory");
    PreparedBudget input_budget;
    Entities copied;
    for (const auto& change:prepared.entity_changes) {
        if (change.kind!=EntityChangeKind::upsert || source.entities().contains(change.entity.id) ||
            (!change.entity_id.empty() && change.entity_id!=change.entity.id))
            reject("fresh graph translation cannot edit an original owner");
        input_budget.entity(change.entity);
        const auto offset=owner_offsets.find(change.entity.id);
        if (offset==owner_offsets.end() || !std::isfinite(offset->second.x) || !std::isfinite(offset->second.y) ||
            std::abs(offset->second.x)>1e12 || std::abs(offset->second.y)>1e12)
            reject("fresh graph translation requires bounded explicit owner offsets");
    }
    for (const auto& change:prepared.entity_changes)
        if (!copied.emplace(change.entity.id,change.entity).second) reject("fresh graph translation repeats an owner");
    if (annotation_offsets.size()>change_limit) reject("fresh annotation offset budget exceeded");
    std::set<std::pair<std::string,std::string>> annotation_children;
    if (!annotation_offsets.empty()) for (const auto& [id,entity]:copied) {
        if (entity.type!=kAnnotationEntityType) continue;
        const auto state=decode_annotation_entity(entity);
        for (const auto& child:state.labels) annotation_children.emplace(id,child.id);
        for (const auto& child:state.symbols) annotation_children.emplace(id,child.id);
    }
    for (const auto& [key,offset]:annotation_offsets) {
        const auto owner=copied.find(key.first);
        if (owner==copied.end() || owner->second.type!=kAnnotationEntityType ||
            !std::isfinite(offset.x) || !std::isfinite(offset.y) || std::abs(offset.x)>1e12 || std::abs(offset.y)>1e12)
            reject("fresh annotation offset requires a bounded actual child target");
        if (!annotation_children.contains(key)) reject("fresh annotation offset has no qualified child");
    }
    std::vector<BoundaryTransformation> boundaries;
    Ids plain_source_owners,boundary_ids;
    for (const auto& [id,entity]:copied) {
        if (!can_recognize_boundary_entity_type(entity.type)) continue;
        const auto offset=owner_offsets.at(id);
        if (entity.type=="measurement_boundary" && entity.extensions.contains("measurement_linework_sources") &&
            !entity.properties.contains("boundary_authoring") && !entity.extensions.contains("boundary_geometry_derivation"))
            plain_source_owners.insert(id);
        else if (offset.x!=0.0 || offset.y!=0.0) {
            boundaries.push_back({id,{{},0,false,false,offset}});boundary_ids.insert(id);
        }
    }
    auto placed=boundaries.empty() ? copied : transformed_boundary_entities_per_owner_batch(copied,boundaries);
    for (const auto& id:plain_source_owners) {
        const auto offset=owner_offsets.at(id);
        if (offset.x==0.0 && offset.y==0.0) continue;
        const auto& original=copied.at(id);
        auto boundary=decode_identified_boundary_entity(original);
        const PlanarTransform transform{{},0,false,false,offset};
        for (auto& edge:boundary.segments) edge.segment=transform_segment(edge.segment,transform);
        placed.at(id)=encode_identified_boundary_entity(boundary,&original);
    }
    const auto physical=stage_fresh_architectural_clipboard_translation(
        source,copied,{},source.revision(),owner_offsets);
    for (const auto& [id,entity]:physical)
        if (!exact(entity,copied.at(id))) placed.at(id)=entity;
    for (auto& [id,entity]:placed) {
        const auto offset=owner_offsets.at(id);
        const PlanarTransform transform{{},0,false,false,offset};
        if (offset.x==0.0 && offset.y==0.0 && entity.type!=kAnnotationEntityType) continue;
        if (entity.type=="measurement_linework") {
            const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
            if (!decoded.supported()) reject("fresh graph has unsupported measured linework");
            const auto moved=transformed_measurement_linework(*decoded.model,transform);
            const Json operation{{"version",1},{"pivot",{transform.pivot.x,transform.pivot.y}},
                {"rotation_radians",transform.rotation_radians},{"flip_horizontal",transform.flip_horizontal},
                {"flip_vertical",transform.flip_vertical},{"offset",{offset.x,offset.y}}};
            // Append only the new derivation to raw source evidence. Encoding
            // the whole model would normalize historical numeric receipt forms.
            auto& raw=entity.properties.at("model");
            if (decoded.model->schema_version>=measurement_linework_schema_version_v3)
                raw.at("operations").push_back({{"type","transform"},{"transform",operation}});
            else {
                if (decoded.model->schema_version==measurement_linework_schema_version_v1) {
                    raw.at("version")=measurement_linework_schema_version_v2;
                    raw.at("replay_version")=measurement_linework_replay_version_v2;
                    raw["transforms"]=Json::array();
                }
                raw.at("transforms").push_back(operation);
            }
            const auto retained=decode_measurement_linework_model(raw);
            if (!retained.supported()) reject("translated raw measured linework is unsupported");
            const auto expected=replay_measurement_linework(moved),actual=replay_measurement_linework(*retained.model);
            if (actual.stroke_id!=expected.stroke_id || actual.closed!=expected.closed ||
                actual.anchor.x!=expected.anchor.x || actual.anchor.y!=expected.anchor.y ||
                actual.replay_version!=expected.replay_version || actual.edges!=expected.edges || actual.receipts!=expected.receipts)
                reject("raw measured translation differs from its typed operation");
        } else if (entity.type=="constraint") {
            const auto decoded=decode_constraint_entity(entity);
            if (!decoded.supported()) reject("fresh graph has an unsupported constraint");
            // Only a fixed anchor owns a translated coordinate. Keep raw
            // bindings, dialect and quantity receipts intact for every relation.
            if (decoded.constraint->anchor) {
                const auto moved=transform_point(*decoded.constraint->anchor,transform);
                auto& anchor=entity.properties.at("anchor_m");
                if (offset.x!=0.0) anchor.at(0)=moved.x;
                if (offset.y!=0.0) anchor.at(1)=moved.y;
                if (!decode_constraint_entity(entity).supported())
                    reject("translated raw constraint is unsupported");
            }
        } else if (entity.type=="dimension") {
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) reject("fresh graph has an unsupported dimension");
            // The boundary kernel already moves its own bound callouts once.
            if (!boundary_ids.contains(decoded.dimension->boundary_id) && exact(entity,copied.at(id))) {
                const auto moved=transform_point(decoded.dimension->text_position,transform);
                auto& position=entity.properties.at("text_position");
                if (offset.x!=0.0) position.at(0)=moved.x;
                if (offset.y!=0.0) position.at(1)=moved.y;
                if (!decode_boundary_dimension_entity(entity).supported())
                    reject("translated raw dimension is unsupported");
            }
        } else if (entity.type==kAnnotationEntityType) {
            (void)decode_annotation_entity(entity);
            for (const auto* collection:{"labels","symbols"})
                for (auto& child:entity.properties.at("state").at(collection)) {
                    auto& placement=child.at("placement");
                    const auto local=annotation_offsets.find({id,child.at("id").get<std::string>()});
                    const auto translation=local==annotation_offsets.end() ? offset : local->second;
                    if (translation.x!=0.0) placement["x"]=placement.at("x").get<double>()+translation.x;
                    if (translation.y!=0.0) placement["y"]=placement.at("y").get<double>()+translation.y;
                }
            validate_annotation_entity(entity);
        } else if (entity.type=="terrain_surface") {
            (void)TerrainSurface::from_json(entity.properties.at("model"));
            for (auto& point:entity.properties.at("model").at("points")) {
                point["x_m"]=point.at("x_m").get<double>()+offset.x;
                point["y_m"]=point.at("y_m").get<double>()+offset.y;
            }
            (void)TerrainSurface::from_json(entity.properties.at("model"));
        } else if (entity.type=="label") {
            auto& point=entity.properties.at("position");
            if (!point.is_array() || (point.size()!=2 && point.size()!=3) ||
                !point.at(0).is_number() || !point.at(1).is_number())
                reject("fresh legacy label requires its explicit plan position");
            const auto position=transform_point({point.at(0).get<double>(),point.at(1).get<double>()},transform);
            if (!std::isfinite(position.x) || !std::isfinite(position.y)) reject("fresh legacy label translation overflows");
            point.at(0)=position.x;point.at(1)=position.y;
        }
    }
    auto result=prepared;
    PreparedBudget output_budget;
    for (auto& change:result.entity_changes) {
        const auto& entity=placed.at(change.entity.id);
        output_budget.entity(entity);change.entity=entity;
    }
    return result;
}
} // namespace sketch
