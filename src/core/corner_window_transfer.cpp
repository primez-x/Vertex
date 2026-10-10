#include "sketch/corner_window_transfer.hpp"

#include "sketch/corner_window.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/quantity.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr double tolerance = default_geometry_tolerance_metres;
// Match the per-object portable Document bounds before copying passive data.
constexpr std::size_t json_byte_limit = 1024 * 1024;
constexpr std::size_t json_value_limit = 100'000;
constexpr std::size_t json_depth_limit = 64;
constexpr std::array context_keys{"property_id", "building_id", "floor_id", "layer_id", "level_id"};
using Ids = std::set<std::string, std::less<>>;
using Remap = std::map<std::string, std::pair<std::string, std::string_view>, std::less<>>;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Corner window transfer: " + reason);
}

bool valid_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        });
}

void validate_json(const Json& value, std::size_t depth, std::size_t& count,
                   std::size_t& text_bytes) {
    if (++count > json_value_limit || depth > json_depth_limit)
        reject("passive JSON exceeds complexity limits");
    if (value.is_discarded() || value.is_binary() ||
        (value.is_number_float() && !std::isfinite(value.get<double>())))
        reject("passive JSON contains a nonportable value");
    const auto add_text = [&](std::size_t bytes) {
        if (bytes > json_byte_limit - text_bytes) reject("passive JSON exceeds the byte limit");
        text_bytes += bytes;
    };
    if (value.is_string()) add_text(value.get_ref<const std::string&>().size());
    else if (value.is_array()) {
        for (const auto& child : value) validate_json(child, depth + 1, count, text_bytes);
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key.size() > 128) reject("passive JSON contains an oversized key");
            add_text(key.size());
            validate_json(child, depth + 1, count, text_bytes);
        }
    }
}

void validate_object(const Json& value) {
    if (!value.is_object()) reject("properties and extensions must be objects");
    std::size_t count = 0, text_bytes = 0;
    validate_json(value, 0, count, text_bytes);
    try {
        if (value.dump().size() > json_byte_limit) reject("passive JSON exceeds the encoded byte limit");
    } catch (const Json::exception&) {
        reject("passive JSON cannot be encoded");
    }
}

void validate_envelope(const Entity& entity, std::string_view type) {
    if (!valid_id(entity.id) || entity.type != type) reject("invalid entity identity or role");
    validate_object(entity.properties);
    validate_object(entity.extensions);
    for (const auto* key : {"id", "type", "required", "properties"})
        if (entity.extensions.contains(key)) reject("extensions shadow a reserved entity field");
    const auto entries = entity.properties.find("quantity_entries");
    if (entries != entity.properties.end() && !entries->is_object())
        reject("quantity_entries must be an object");
    if (type != "opening" && (entity.properties.contains("corner_window_id") ||
                              entity.properties.contains("corner_leg")))
        reject("only managed cuts may carry corner backlinks");
}

const Json& optional(const Entity& entity, const char* key) {
    static const Json absent;
    const auto found = entity.properties.find(key);
    return found == entity.properties.end() ? absent : *found;
}

void validate_context(const Entity& entity, const Entity& first_host) {
    for (const auto* key : context_keys) {
        const auto& actual = optional(entity, key);
        const auto& expected = optional(first_host, key);
        if (actual != expected || (entity.properties.contains(key) &&
            (!actual.is_string() || !valid_id(actual.get_ref<const std::string&>()))))
            reject("aggregate requires identical valid raw drawing context and level references");
    }
}

void validate_host_placement(const Entity& host) {
    const auto found = host.properties.find("vertical_placement");
    if (found == host.properties.end()) return;
    const auto& placement = *found;
    if (!placement.is_object() || placement.size() != 3 ||
        !placement.contains("version") || !placement.at("version").is_number_integer() ||
        placement.at("version") != 1 || !placement.contains("mode") ||
        !placement.at("mode").is_string() || !placement.contains("offset_m") ||
        !placement.at("offset_m").is_number())
        reject("unsupported host vertical placement");
    const auto& mode = placement.at("mode").get_ref<const std::string&>();
    const double offset = placement.at("offset_m").get<double>();
    if (!std::isfinite(offset) || std::abs(offset) > 1e9 ||
        (mode != "absolute" && mode != "level") ||
        (mode == "level" && (!host.properties.contains("floor_id") ||
                             !host.properties.contains("elevation_m"))))
        reject("unsupported host vertical placement");
    // The passive packet intentionally contains no floor/level graph. Actual
    // resolution is performed by the destination's full Document/native checks.
}

std::array<Wall, 2> decode_hosts(const std::array<Entity, 2>& entities) {
    std::array<Wall, 2> walls;
    for (std::size_t leg = 0; leg < walls.size(); ++leg) {
        validate_envelope(entities[leg], "wall");
        validate_context(entities[leg], entities[0]);
        validate_host_placement(entities[leg]);
        if (optional(entities[leg], "vertical_placement") != optional(entities[0], "vertical_placement"))
            reject("hosts require identical retained vertical placement");
        std::string error;
        if (!read_document_wall(entities[leg], {}, walls[leg], error)) reject(error);
        validate_wall_semantics(walls[leg]);
    }
    return walls;
}

void validate_profile_fit(const CornerWindow& value, const std::array<Wall, 2>& walls) {
    // Structural admission only: native common-post, panes, sibling cuts and
    // wall remnants are checked against the completed destination map later.
    for (const auto& wall : walls)
        if (value.assembly.frame_depth_m > wall.thickness + tolerance ||
            std::abs(value.assembly.inset_m) + value.assembly.frame_depth_m * 0.5 >
                wall.thickness * 0.5 + tolerance)
            reject("fixed profile does not fit both host thicknesses");
}

void match_dimension(const Json& properties, const char* canonical, const char* alias,
                     double expected) {
    bool present = false;
    for (const auto* key : {canonical, alias}) {
        const auto found = properties.find(key);
        if (found == properties.end()) continue;
        present = true;
        if (!found->is_number() || !std::isfinite(found->get<double>()) || found->get<double>() != expected)
            reject(std::string("cut ") + key + " differs from its owner's derived geometry");
    }
    if (!present) reject(std::string("cut is missing ") + canonical);
}

void retain_dimension(Json& properties, const char* canonical, const char* alias, double value) {
    // Validation proved at least one name exists. Do not introduce another
    // numeric representation when an unchanged or legacy-only value survives.
    for (const auto* key : {canonical, alias}) {
        const auto found = properties.find(key);
        if (found != properties.end() && found->get<double>() != value) *found = value;
    }
}

void inherit_context(Entity& entity, const Entity& host) {
    for (const auto* key : context_keys) {
        const auto found = host.properties.find(key);
        if (found == host.properties.end()) entity.properties.erase(key);
        else entity.properties[key] = *found;
    }
}

void retarget_references(Entity& entity, const Remap& remap) {
    // These are the top-level entity/asset slots collected by Document. Unknown
    // fields, nested metadata, extensions and equal incidental strings remain
    // untouched; a passive packet cannot prove external canonical authority.
    constexpr std::array<std::pair<std::string_view, std::string_view>, 20> typed{{
        {"assembly_catalog_id", "assembly_model"}, {"property_id", "property"},
        {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"},
        {"boundary_id", "boundary"}, {"wall_id", "wall"}, {"opening_id", "opening"},
        {"corner_window_id", "corner_window"}, {"room_id", "room"}, {"slab_id", "slab"},
        {"roof_id", "roof"}, {"stair_id", "stair"}, {"sheet_id", "sheet"}, {"view_id", "view"},
        {"constraint_id", "constraint"}, {"label_id", "label"}, {"column_id", "column"},
        {"beam_id", "beam"}, {"railing_id", "railing"}}};
    const auto write = [&](Json& reference, std::string_view expected) {
        if (!reference.is_string()) reject("canonical reference must contain an identity string");
        const auto found = remap.find(reference.get_ref<const std::string&>());
        if (found == remap.end()) reject("external canonical references require explicit transport");
        if (!expected.empty() && found->second.second != expected)
            reject("canonical reference targets the wrong aggregate role");
        reference = found->second.first;
    };
    for (auto& [key, value] : entity.properties.items()) {
        if (std::find(context_keys.begin(), context_keys.end(), key) != context_keys.end()) continue;
        bool known = key == "refs" || key == "references" || key == "parent_id" || key == "parent_ids" ||
            key == "host_id" || key == "host_ids" || key == "target_id" || key == "target_ids" ||
            key == "entity_id" || key == "entity_ids" || key == "source_entity_id" || key == "source_entity_ids";
        std::string_view expected;
        for (const auto& [singular, type] : typed)
            if (key == singular || (key.size() == singular.size() + 1 && key.back() == 's' &&
                                   std::string_view(key).substr(0, singular.size()) == singular)) {
                known = true;
                expected = type;
                break;
            }
        if (key == "asset_id" || key == "asset_ids" || key == "render_asset_id" || key == "render_asset_ids")
            reject("canonical asset references require explicit asset transport");
        if (!known) continue;
        const bool collection = key == "refs" || key == "references" || std::string_view(key).ends_with("_ids");
        if (!collection) write(value, expected);
        else {
            if (!value.is_array()) reject("canonical reference collection must be an array");
            for (auto& reference : value) write(reference, expected);
        }
    }
}

struct IdentityReservation {
    const Ids& fresh;
    std::size_t nodes{}, bytes{};
    static constexpr std::size_t node_limit = 4 * 1024 * 1024, byte_limit = 64 * 1024 * 1024;

    void text(std::string_view value) {
        if (value.size() > byte_limit - bytes) reject("retained identity string budget exceeded");
        bytes += value.size();
        if (fresh.contains(value)) reject("fresh identity was already retained in source/history");
    }
    void read(const Json& value, unsigned depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("retained identity JSON budget exceeded");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
    void entity(const Entity& value) {
        if (++nodes > node_limit) reject("retained entity row budget exceeded");
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
    void changes(const std::vector<EntityChange>& values) {
        if (values.size() > node_limit - nodes) reject("retained change row budget exceeded");
        for (const auto& change : values) {
            ++nodes;
            if (change.kind == EntityChangeKind::upsert) entity(change.entity);
            else text(change.entity_id);
        }
    }
};

void reserve_identities(const DocumentSnapshot& destination, const CornerWindowTransfer& transfer,
                        const Ids& fresh) {
    const auto& history = destination.history();
    if (history.empty() || history.size() > 4096 || destination.revision() >= history.size())
        reject("destination retained history exceeds admission bounds");
    IdentityReservation reservation{fresh};
    // Match the conservative source-envelope reservation used by physical
    // replacements, without decoding unknown metadata or rewriting any string.
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) reservation.text(key);
    reservation.entity(transfer.owner);
    for (const auto& host : transfer.walls) reservation.entity(host);
    for (const auto& cut : transfer.cuts) reservation.entity(cut);
    // Scan every saved record, including records beyond the current Undo head.
    // Ordinary ApplyEntityChanges retains resulting maps rather than commands;
    // typed change lanes and raw intents may also carry retired source envelopes.
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& record = history[index];
        if (record.revision != index) reject("destination history is not contiguous");
        for (const auto& [id, entity] : record.entities) {
            reservation.text(id); reservation.entity(entity);
        }
        for (const auto& [id, asset] : record.assets) {
            if (++reservation.nodes > IdentityReservation::node_limit) reject("retained asset row budget exceeded");
            reservation.text(id); reservation.text(asset.id); reservation.read(asset.metadata);
        }
        if (record.boundary_translations) reservation.changes(record.boundary_translations->entity_changes);
        if (record.boundary_transforms) reservation.changes(record.boundary_transforms->entity_changes);
        if (record.boundary_constraint_changes) {
            const auto& proof = *record.boundary_constraint_changes;
            reservation.changes(proof.entity_changes);
            reservation.changes(proof.physical_entity_changes);
            reservation.changes(proof.supplemental_entity_changes);
            reservation.changes(proof.selection_entity_changes);
            if (proof.rigid_group_transform) reservation.changes(proof.rigid_group_transform->entity_changes);
            for (const auto* payload : {&proof.room_review_intent, &proof.room_review_geometry_proof,
                &proof.phase_room_review_intent, &proof.phase_constraint_authoring_intent,
                &proof.independent_drawing_removal_intent}) reservation.read(*payload);
            if (proof.room_review_additional_intents.size() > IdentityReservation::node_limit - reservation.nodes)
                reject("retained review-intent row budget exceeded");
            for (const auto& payload : proof.room_review_additional_intents) reservation.read(payload);
        }
        if (record.phase_entity_import) for (const auto& id : record.phase_entity_import->entity_ids) {
            if (++reservation.nodes > IdentityReservation::node_limit) reject("retained import row budget exceeded");
            reservation.text(id);
        }
    }
}

Json numeric_receipt(double metres) {
    std::array<char, 64> buffer{};
    // As with roof-opening replay, shorten an int64-overflowing decimal only
    // when reparsing it reproduces the exact actual double station.
    for (int precision = 17; precision > 0; --precision) {
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
            metres, std::chars_format::general, precision);
        if (converted.ec != std::errc{}) continue;
        try {
            const auto value = parse_quantity(std::string(buffer.data(), converted.ptr) + " m", Unit::metre);
            if (value.metres != metres) continue;
            // The fixed-length encoder excludes zero, while a cut station
            // may be zero. Use the shared decoder to admit this numeric core.
            Json result{{"version", 1}, {"original_expression", value.original_expression},
                {"entered_unit", "m"}, {"exact_metres", {
                    {"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
            if (decode_constraint_quantity_receipt(result).metres == metres) return result;
        } catch (const std::invalid_argument&) { /* Try a shorter exact bounded spelling. */ }
          catch (const std::overflow_error&) { /* A shorter decimal may fit the rational range. */ }
    }
    reject("changed cut station has no exact bounded metre receipt");
}

void validate_supported_quantity_entries(const Entity& entity,
                                         std::initializer_list<const char*> pointers) {
    const auto entries = entity.properties.find("quantity_entries");
    if (entries == entity.properties.end()) return;
    for (const auto* pointer : pointers) {
        const auto found = entries->find(pointer);
        if (found == entries->end() || !found->is_object()) continue;
        const auto& receipt = *found;
        const auto version = receipt.find("version");
        if (version == receipt.end() || !version->is_number_integer() || *version != 1) continue;
        const auto exact = receipt.find("exact_metres");
        // A declared v1 numeric core, even if partially malformed, must be
        // admitted. Pure annotations and unrecognized future envelopes remain
        // opaque when unchanged; replay refuses them if their scalar changes.
        const bool numeric_core = receipt.contains("original_expression") || receipt.contains("entered_unit") ||
            (exact != receipt.end() && (!exact->is_object() || exact->contains("numerator") || exact->contains("denominator")));
        if (!numeric_core) continue;
        const Json::json_pointer path(pointer);
        if (!entity.properties.contains(path) || !entity.properties.at(path).is_number())
            reject("supported quantity receipt has no source scalar binding");
        if (!receipt.contains("original_expression") || !receipt.at("original_expression").is_string() ||
            receipt.at("original_expression").get_ref<const std::string&>().size() > 4096)
            reject("supported quantity receipt lacks a bounded expression");
        Quantity decoded;
        try {
            decoded = decode_constraint_quantity_receipt(receipt);
        } catch (const std::invalid_argument&) {
            reject("supported quantity receipt has a malformed numeric core");
        }
        if (decoded.metres != entity.properties.at(path).get<double>())
            reject("supported quantity receipt is stale against its source scalar");
    }
}

void replay_changed_quantity_entries(const Entity& before, Entity& after) {
    const auto entries = before.properties.find("quantity_entries");
    if (entries == before.properties.end()) return;
    Json retained = *entries;
    // Only understood changed scalar bindings are replayed. Future/core-free
    // affected receipts refuse; unrelated pointers and unchanged cores remain
    // opaque. Updating the core preserves annotation siblings and nested
    // exact_metres metadata instead of discarding the receipt envelope.
    for (const auto* pointer : {"/offset_m", "/offset", "/width_m", "/width",
                                "/sill_m", "/sill", "/height_m", "/height"}) {
        const Json::json_pointer path(pointer);
        if (!retained.contains(pointer) || !before.properties.contains(path) ||
            (after.properties.contains(path) && before.properties.at(path) == after.properties.at(path)))
            continue;
        if (!after.properties.contains(path) || !after.properties.at(path).is_number())
            reject("changed cut receipt has no numeric destination");
        auto& receipt = retained.at(pointer);
        if (!receipt.is_object() || !receipt.contains("original_expression") ||
            !receipt.at("original_expression").is_string() ||
            receipt.at("original_expression").get_ref<const std::string&>().size() > 4096)
            reject("changed cut receipt has no supported bounded numeric core");
        try {
            if (decode_constraint_quantity_receipt(receipt).metres != before.properties.at(path).get<double>())
                reject("changed cut receipt is stale");
        } catch (const std::invalid_argument&) {
            reject("changed cut receipt requires a current supported numeric core");
        }
        const auto encoded = numeric_receipt(after.properties.at(path).get<double>());
        for (const auto* field : {"version", "original_expression", "entered_unit"})
            receipt[field] = encoded.at(field);
        for (const auto* field : {"numerator", "denominator"})
            receipt["exact_metres"][field] = encoded.at("exact_metres").at(field);
    }
    after.properties["quantity_entries"] = std::move(retained);
}
} // namespace

void validate_corner_window_transfer(const CornerWindowTransfer& transfer) {
    validate_envelope(transfer.owner, "corner_window");
    const auto value = parse_corner_window(transfer.owner);
    validate_supported_quantity_entries(transfer.owner, {"/widths_m/0", "/widths_m/1", "/sill_m", "/height_m",
        "/opening_assembly/frame_width_m", "/opening_assembly/frame_depth_m", "/opening_assembly/panel_thickness_m",
        "/opening_assembly/glazing_thickness_m", "/opening_assembly/inset_m"});
    const auto walls = decode_hosts(transfer.walls);
    validate_context(transfer.owner, transfer.walls[0]);
    if (transfer.owner.properties.contains("vertical_placement"))
        reject("owner inherits host elevation");
    const auto cuts = corner_window_cuts(value, walls);
    validate_profile_fit(value, walls);
    for (std::size_t leg = 0; leg < transfer.cuts.size(); ++leg) {
        const auto& child = transfer.cuts[leg];
        validate_envelope(child, "opening");
        validate_context(child, transfer.walls[0]);
        const auto& p = child.properties;
        if (child.id != value.opening_ids[leg] || !p.contains("wall_id") ||
            p.at("wall_id") != value.wall_ids[leg] || !p.contains("corner_window_id") ||
            p.at("corner_window_id") != value.id || !p.contains("corner_leg") ||
            !p.at("corner_leg").is_number_integer() || p.at("corner_leg") != leg ||
            !p.contains("opening_kind") || p.at("opening_kind") != "opening" ||
            p.contains("opening_assembly") || p.contains("door_operation") || p.contains("vertical_placement"))
            reject("cut must be its owner's ordered reciprocal bare opening");
        match_dimension(p, "offset_m", "offset", cuts[leg].offset);
        match_dimension(p, "width_m", "width", cuts[leg].width);
        match_dimension(p, "sill_m", "sill", cuts[leg].sill);
        match_dimension(p, "height_m", "height", cuts[leg].height);
        validate_supported_quantity_entries(child, {"/offset_m", "/offset", "/width_m", "/width",
            "/sill_m", "/sill", "/height_m", "/height"});
    }
    // Copy/Cut uses this admission before publishing or deleting its source.
    // Exercise the same canonical transport policy with an identity map, so
    // unsupported external bindings cannot create an unpasteable clipboard.
    const Remap self{{transfer.owner.id, {transfer.owner.id, "corner_window"}},
        {transfer.cuts[0].id, {transfer.cuts[0].id, "opening"}},
        {transfer.cuts[1].id, {transfer.cuts[1].id, "opening"}},
        {transfer.walls[0].id, {transfer.walls[0].id, "wall"}},
        {transfer.walls[1].id, {transfer.walls[1].id, "wall"}}};
    auto owner = transfer.owner;
    retarget_references(owner, self);
    for (auto child : transfer.cuts) retarget_references(child, self);
}

ApplyEntityChanges corner_window_clone_command(const DocumentSnapshot& destination,
    const CornerWindowTransfer& transfer, const std::string& owner_id,
    const std::array<std::string, 2>& opening_ids, const std::array<std::string, 2>& wall_ids,
    const std::array<bool, 2>& at_start, Revision expected_revision) {
    if (!destination.is_editable() || destination.revision() != expected_revision)
        reject("destination is read-only or stale");
    validate_corner_window_transfer(transfer);
    const std::set<std::string, std::less<>> passive_ids{transfer.owner.id,
        transfer.walls[0].id, transfer.walls[1].id, transfer.cuts[0].id, transfer.cuts[1].id};
    std::set<std::string, std::less<>> fresh_ids;
    for (const auto& id : {owner_id, opening_ids[0], opening_ids[1]})
        if (!valid_id(id) || !fresh_ids.insert(id).second || passive_ids.contains(id) ||
            destination.entities().contains(id) || id == wall_ids[0] || id == wall_ids[1])
            reject("owner and cuts require distinct fresh destination identities");
    reserve_identities(destination, transfer, fresh_ids);
    std::array<Entity, 2> host_entities;
    for (std::size_t leg = 0; leg < host_entities.size(); ++leg) {
        const auto found = destination.entities().find(wall_ids[leg]);
        if (!valid_id(wall_ids[leg]) || found == destination.entities().end() ||
            found->second.id != wall_ids[leg] || found->second.type != "wall")
            reject("destination requires two actual wall hosts");
        host_entities[leg] = found->second;
    }
    const auto walls = decode_hosts(host_entities);
    const Remap remap{{transfer.owner.id, {owner_id, "corner_window"}},
        {transfer.cuts[0].id, {opening_ids[0], "opening"}}, {transfer.cuts[1].id, {opening_ids[1], "opening"}},
        {transfer.walls[0].id, {wall_ids[0], "wall"}}, {transfer.walls[1].id, {wall_ids[1], "wall"}}};
    Entity owner = transfer.owner;
    retarget_references(owner, remap);
    owner.id = owner_id;
    owner.properties["wall_ids"] = wall_ids;
    owner.properties["opening_ids"] = opening_ids;
    owner.properties["at_start"] = at_start;
    inherit_context(owner, host_entities[0]);
    const auto value = parse_corner_window(owner);
    const auto cuts = corner_window_cuts(value, walls);
    validate_profile_fit(value, walls);
    ApplyEntityChanges command{expected_revision, {}, {}, "Clone corner window"};
    command.entity_changes.reserve(3);
    command.entity_changes.push_back(EntityChange::upsert(std::move(owner)));
    for (std::size_t leg = 0; leg < cuts.size(); ++leg) {
        Entity child = transfer.cuts[leg];
        retarget_references(child, remap);
        child.id = opening_ids[leg];
        child.properties["wall_id"] = wall_ids[leg];
        child.properties["corner_window_id"] = owner_id;
        inherit_context(child, host_entities[leg]);
        retain_dimension(child.properties, "offset_m", "offset", cuts[leg].offset);
        retain_dimension(child.properties, "width_m", "width", cuts[leg].width);
        retain_dimension(child.properties, "sill_m", "sill", cuts[leg].sill);
        retain_dimension(child.properties, "height_m", "height", cuts[leg].height);
        replay_changed_quantity_entries(transfer.cuts[leg], child);
        command.entity_changes.push_back(EntityChange::upsert(std::move(child)));
    }
    return command;
}
} // namespace sketch
