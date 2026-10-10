#include "sketch/corner_window_transfer.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_model.hpp"
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
constexpr std::size_t dimension_row_limit = 2048;
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
                   std::size_t& text_bytes, std::size_t byte_limit = json_byte_limit) {
    if (++count > json_value_limit || depth > json_depth_limit)
        reject("passive JSON exceeds complexity limits");
    if (value.is_discarded() || value.is_binary() ||
        (value.is_number_float() && !std::isfinite(value.get<double>())))
        reject("passive JSON contains a nonportable value");
    const auto add_text = [&](std::size_t bytes) {
        if (bytes > byte_limit - text_bytes) reject("passive JSON exceeds the byte limit");
        text_bytes += bytes;
    };
    if (value.is_string()) add_text(value.get_ref<const std::string&>().size());
    else if (value.is_array()) {
        for (const auto& child : value) validate_json(child, depth + 1, count, text_bytes, byte_limit);
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key.size() > 128) reject("passive JSON contains an oversized key");
            add_text(key.size());
            validate_json(child, depth + 1, count, text_bytes, byte_limit);
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

void reserve_identities(const DocumentSnapshot& destination, const std::vector<const CornerWindowTransfer*>& transfers,
                        const Ids& fresh, const std::vector<Entity>& imported_catalogs = {}) {
    const auto& history = destination.history();
    if (history.empty() || history.size() > 4096 || destination.revision() >= history.size())
        reject("destination retained history exceeds admission bounds");
    Ids all_fresh = fresh, imported_ids;
    for (const auto& catalog : imported_catalogs) {
        if (!valid_id(catalog.id) || !all_fresh.insert(catalog.id).second)
            reject("imported catalog identities must be distinct and fresh");
        imported_ids.insert(catalog.id);
    }
    IdentityReservation reservation{all_fresh};
    // Match the conservative source-envelope reservation used by physical
    // replacements, without decoding unknown metadata or rewriting any string.
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) reservation.text(key);
    const auto reserve_transported_material_entity = [&](const Entity& entity) {
        if (imported_ids.empty()) { reservation.entity(entity); return; }
        // Remapped typed catalog references intentionally name a new import.
        // Validate those sites, then exclude only their value from this scan;
        // all opaque data, keys and material names still reserve identities.
        (void)architectural_material_source_refs(entity);
        auto scanned = entity;
        auto assignment = scanned.properties.find("material_assignment");
        if (assignment != scanned.properties.end() && assignment->contains("catalog_id") &&
            assignment->at("catalog_id").is_string() &&
            imported_ids.contains(assignment->at("catalog_id").get<std::string>()))
            assignment->at("catalog_id") = nullptr;
        reservation.entity(scanned);
    };
    for (const auto* transfer : transfers) {
        reserve_transported_material_entity(transfer->owner);
        for (const auto& host : transfer->walls) reservation.entity(host);
        for (const auto& cut : transfer->cuts) reserve_transported_material_entity(cut);
        for (const auto& dimension : transfer->dimensions) reservation.entity(dimension);
    }
    for (const auto& catalog : imported_catalogs) {
        for (const auto* transfer : transfers) {
            if (catalog.id == transfer->owner.id) reject("catalog identity overlaps a source owner");
            for (const auto& host : transfer->walls) if (catalog.id == host.id) reject("catalog identity overlaps a source host");
            for (const auto& cut : transfer->cuts) if (catalog.id == cut.id) reject("catalog identity overlaps a source cut");
            for (const auto& dimension : transfer->dimensions) if (catalog.id == dimension.id) reject("catalog identity overlaps a source dimension");
        }
        reservation.text(catalog.type); reservation.read(catalog.properties); reservation.read(catalog.extensions);
    }
    auto& history_reservation = reservation;
    // Scan every saved record, including records beyond the current Undo head.
    // Ordinary ApplyEntityChanges retains resulting maps rather than commands;
    // typed change lanes and raw intents may also carry retired source envelopes.
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& record = history[index];
        if (record.revision != index) reject("destination history is not contiguous");
        for (const auto& [id, entity] : record.entities) {
            history_reservation.text(id); history_reservation.entity(entity);
        }
        for (const auto& [id, asset] : record.assets) {
            if (++history_reservation.nodes > IdentityReservation::node_limit) reject("retained asset row budget exceeded");
            history_reservation.text(id); history_reservation.text(asset.id); history_reservation.read(asset.metadata);
        }
        if (record.boundary_translations) history_reservation.changes(record.boundary_translations->entity_changes);
        if (record.boundary_transforms) history_reservation.changes(record.boundary_transforms->entity_changes);
        if (record.boundary_constraint_changes) {
            const auto& proof = *record.boundary_constraint_changes;
            history_reservation.changes(proof.entity_changes);
            history_reservation.changes(proof.physical_entity_changes);
            history_reservation.changes(proof.supplemental_entity_changes);
            history_reservation.changes(proof.selection_entity_changes);
            if (proof.rigid_group_transform) history_reservation.changes(proof.rigid_group_transform->entity_changes);
            for (const auto* payload : {&proof.room_review_intent, &proof.room_review_geometry_proof,
                &proof.phase_room_review_intent, &proof.phase_constraint_authoring_intent,
                &proof.independent_drawing_removal_intent}) history_reservation.read(*payload);
            if (proof.room_review_additional_intents.size() > IdentityReservation::node_limit - history_reservation.nodes)
                reject("retained review-intent row budget exceeded");
            for (const auto& payload : proof.room_review_additional_intents) history_reservation.read(payload);
        }
        if (record.phase_entity_import) for (const auto& id : record.phase_entity_import->entity_ids) {
            if (++history_reservation.nodes > IdentityReservation::node_limit) reject("retained import row budget exceeded");
            history_reservation.text(id);
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

BoundaryDimension decode_corner_dimension(const Entity& entity, const Entity& owner) {
    validate_envelope(entity, "dimension");
    const auto decoded = decode_boundary_dimension_entity(entity);
    if (!decoded.supported() || decoded.dimension->kind != BoundaryDimensionKind::corner_window_leg_length ||
        decoded.dimension->boundary_id != owner.id)
        reject("transported dimensions must have a supported corner-leg target bound to the actual owner");
    // This checks the model and actual owner identity without pretending the
    // passive five-part packet contains an authoritative floor/phase graph.
    validate_boundary_dimension_target(*decoded.dimension, owner);
    return *decoded.dimension;
}

std::array<Segment, 2> actual_leg_segments(const Entity& owner,
    const std::array<Entity, 2>& hosts, const std::array<Entity, 2>& children) {
    const auto corner = parse_corner_window(owner);
    std::array<Wall, 2> walls;
    for (std::size_t leg = 0; leg < 2; ++leg) {
        std::string error;
        if (!read_document_wall(hosts[leg], {&children[leg]}, walls[leg], error)) reject(error);
        validate_wall_semantics(walls[leg]);
    }
    // Derive from real source envelopes and decoded cuts, never owner widths
    // alone or synthetic boundary IDs. Raw XY needs no imported level graph.
    (void)corner_window_cuts(corner, walls);
    std::array<Segment, 2> result;
    for (std::size_t leg = 0; leg < 2; ++leg) {
        const auto& wall = walls[leg];
        if (wall.openings.size() != 1 || wall.openings.front().id != corner.opening_ids[leg])
            reject("dimension leg requires its actual ordered owned cut");
        const auto& cut = wall.openings.front();
        const auto length = segment_length(wall.baseline);
        const auto station = corner.at_start[leg] ? cut.offset + cut.width : cut.offset;
        const Vec2 jamb{
            wall.baseline.start.x + (wall.baseline.end.x - wall.baseline.start.x) * station / length,
            wall.baseline.start.y + (wall.baseline.end.y - wall.baseline.start.y) * station / length};
        result[leg] = {corner.at_start[leg] ? wall.baseline.start : wall.baseline.end, jamb, 0.0};
        const auto span = segment_length(result[leg]);
        if (!std::isfinite(jamb.x) || !std::isfinite(jamb.y) || !std::isfinite(span) || span <= tolerance)
            reject("dimension leg has no finite measurable endpoint-to-jamb span");
    }
    return result;
}

void carry_dimension_placement(BoundaryDimension& dimension, const Segment& source,
                               const Segment& destination) {
    if (source.start.x == destination.start.x && source.start.y == destination.start.y &&
        source.end.x == destination.end.x && source.end.y == destination.end.y) return;
    const auto source_length = segment_length(source);
    const auto destination_length = segment_length(destination);
    const Vec2 from{(source.end.x - source.start.x) / source_length,
                    (source.end.y - source.start.y) / source_length};
    const Vec2 to{(destination.end.x - destination.start.x) / destination_length,
                  (destination.end.y - destination.start.y) / destination_length};
    const Vec2 offset{dimension.text_position.x - source.start.x,
                      dimension.text_position.y - source.start.y};
    const auto along = offset.x * from.x + offset.y * from.y;
    const auto normal = -offset.x * from.y + offset.y * from.x;
    dimension.text_position = {destination.start.x + along * to.x - normal * to.y,
                               destination.start.y + along * to.y + normal * to.x};
    const auto rotation = std::atan2(from.x * to.y - from.y * to.x, from.x * to.x + from.y * to.y);
    if (rotation != 0.0) {
        if (!dimension.presentation) dimension.presentation = BoundaryDimensionPresentation{};
        dimension.presentation->rotation_radians += rotation;
    }
    if (!std::isfinite(dimension.text_position.x) || !std::isfinite(dimension.text_position.y) ||
        (dimension.presentation && !std::isfinite(dimension.presentation->rotation_radians)))
        reject("dimension placement cannot be represented in the destination leg frame");
}
} // namespace

void validate_corner_window_transfer(const CornerWindowTransfer& transfer) {
    if (transfer.dimensions.size() > dimension_row_limit)
        reject("dimension transport exceeds the row limit");
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
    Remap self{{transfer.owner.id, {transfer.owner.id, "corner_window"}},
        {transfer.cuts[0].id, {transfer.cuts[0].id, "opening"}},
        {transfer.cuts[1].id, {transfer.cuts[1].id, "opening"}},
        {transfer.walls[0].id, {transfer.walls[0].id, "wall"}},
        {transfer.walls[1].id, {transfer.walls[1].id, "wall"}}};
    for (const auto& dimension : transfer.dimensions) {
        (void)decode_corner_dimension(dimension, transfer.owner);
        validate_context(dimension, transfer.walls[0]);
        if (!self.emplace(dimension.id, std::pair{dimension.id, std::string_view{"dimension"}}).second)
            reject("transported entity identities must be distinct");
    }
    if (!transfer.dimensions.empty())
        (void)actual_leg_segments(transfer.owner, transfer.walls, transfer.cuts);
    auto owner = transfer.owner;
    retarget_references(owner, self);
    for (auto child : transfer.cuts) retarget_references(child, self);
    for (auto dimension : transfer.dimensions) retarget_references(dimension, self);
}

static ApplyEntityChanges clone_command(const DocumentSnapshot& destination,
    const CornerWindowTransfer& transfer, const std::string& owner_id,
    const std::array<std::string, 2>& opening_ids, const std::array<std::string, 2>& wall_ids,
    const std::array<bool, 2>& at_start, Revision expected_revision,
    const std::map<std::string, std::string, std::less<>>& dimension_ids, bool reserve_history,
    const std::map<std::string, Entity, std::less<>>* fresh_hosts = nullptr) {
    if (!destination.is_editable() || destination.revision() != expected_revision)
        reject("destination is read-only or stale");
    validate_corner_window_transfer(transfer);
    std::set<std::string, std::less<>> passive_ids{transfer.owner.id,
        transfer.walls[0].id, transfer.walls[1].id, transfer.cuts[0].id, transfer.cuts[1].id};
    for (const auto& dimension : transfer.dimensions) passive_ids.insert(dimension.id);
    if (dimension_ids.size() != transfer.dimensions.size())
        reject("dimension identities must cover exactly the transported dimensions");
    std::set<std::string, std::less<>> fresh_ids;
    for (const auto& id : {owner_id, opening_ids[0], opening_ids[1]})
        if (!valid_id(id) || !fresh_ids.insert(id).second || passive_ids.contains(id) ||
            destination.entities().contains(id) || id == wall_ids[0] || id == wall_ids[1])
            reject("owner and cuts require distinct fresh destination identities");
    for (const auto& dimension : transfer.dimensions) {
        const auto found = dimension_ids.find(dimension.id);
        if (found == dimension_ids.end()) reject("transported dimension lacks its explicit fresh identity");
        const auto& id = found->second;
        if (!valid_id(id) || !fresh_ids.insert(id).second || passive_ids.contains(id) ||
            destination.entities().contains(id) || id == wall_ids[0] || id == wall_ids[1])
            reject("dimensions require distinct fresh destination identities");
    }
    if (reserve_history) reserve_identities(destination, {&transfer}, fresh_ids);
    std::array<Entity, 2> host_entities;
    for (std::size_t leg = 0; leg < host_entities.size(); ++leg) {
        const auto found = destination.entities().find(wall_ids[leg]);
        const Entity* host = found == destination.entities().end() ? nullptr : &found->second;
        if (fresh_hosts) {
            const auto prepared = fresh_hosts->find(wall_ids[leg]);
            if (prepared != fresh_hosts->end()) {
                if (host) reject("fresh wall host overlaps an actual destination entity");
                host = &prepared->second;
            }
        }
        if (!valid_id(wall_ids[leg]) || !host || host->id != wall_ids[leg] || host->type != "wall")
            reject("destination requires two actual wall hosts");
        host_entities[leg] = *host;
    }
    const auto walls = decode_hosts(host_entities);
    Remap remap{{transfer.owner.id, {owner_id, "corner_window"}},
        {transfer.cuts[0].id, {opening_ids[0], "opening"}}, {transfer.cuts[1].id, {opening_ids[1], "opening"}},
        {transfer.walls[0].id, {wall_ids[0], "wall"}}, {transfer.walls[1].id, {wall_ids[1], "wall"}}};
    for (const auto& [source_id, fresh_id] : dimension_ids)
        remap.emplace(source_id, std::pair{fresh_id, std::string_view{"dimension"}});
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
    command.entity_changes.reserve(3 + transfer.dimensions.size());
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
    if (!transfer.dimensions.empty()) {
        const auto source_legs = actual_leg_segments(transfer.owner, transfer.walls, transfer.cuts);
        const std::array<Entity, 2> destination_cuts{command.entity_changes[1].entity,
                                                   command.entity_changes[2].entity};
        const auto destination_legs = actual_leg_segments(command.entity_changes[0].entity,
                                                          host_entities, destination_cuts);
        for (const auto& source : transfer.dimensions) {
            auto dimension = decode_corner_dimension(source, transfer.owner);
            const auto source_dimension = dimension;
            const auto leg = *dimension.corner_leg;
            dimension.id = dimension_ids.at(source.id);
            dimension.boundary_id = owner_id;
            carry_dimension_placement(dimension, source_legs[leg], destination_legs[leg]);
            Entity retained = source;
            retained.id = dimension.id;
            retarget_references(retained, remap);
            inherit_context(retained, host_entities[0]);
            auto child = encode_boundary_dimension_entity(dimension, &retained);
            // The typed encoder validates changed semantics. Preserve exact
            // retained numeric forms for unchanged target/presentation fields,
            // including integer height/rotation spellings in existing v5 data.
            child.properties["dimension_version"] = source.properties.at("dimension_version");
            child.properties["target"]["corner_leg"] = source.properties.at("target").at("corner_leg");
            if (source_dimension.automatic_placement_version)
                child.properties["automatic_placement_version"] = source.properties.at("automatic_placement_version");
            if (source_dimension.presentation) {
                child.properties["presentation"] = source.properties.at("presentation");
                if (dimension.presentation->rotation_radians != source_dimension.presentation->rotation_radians)
                    child.properties["presentation"]["rotation_radians"] = dimension.presentation->rotation_radians;
            }
            (void)decode_corner_dimension(child, command.entity_changes[0].entity);
            command.entity_changes.push_back(EntityChange::upsert(std::move(child)));
        }
    }
    return command;
}

ApplyEntityChanges corner_window_clone_command(const DocumentSnapshot& destination,
    const CornerWindowTransfer& transfer, const std::string& owner_id,
    const std::array<std::string, 2>& opening_ids, const std::array<std::string, 2>& wall_ids,
    const std::array<bool, 2>& at_start, Revision expected_revision,
    const std::map<std::string, std::string, std::less<>>& dimension_ids) {
    return clone_command(destination, transfer, owner_id, opening_ids, wall_ids, at_start,
        expected_revision, dimension_ids, true);
}

struct GroupBudget {
    std::size_t values{}, text_bytes{}, encoded_bytes{};
};

static void validate_material_catalog_group(const std::vector<Entity>& catalogs, GroupBudget& budget) {
    if (catalogs.size() > 128) reject("group catalog limit exceeded");
    std::map<std::string, const Entity*, std::less<>> pooled;
    for (const auto& catalog : catalogs) {
        validate_envelope(catalog, "assembly_model");
        validate_json(catalog.properties, 0, budget.values, budget.text_bytes, 4 * 1024 * 1024);
        validate_json(catalog.extensions, 0, budget.values, budget.text_bytes, 4 * 1024 * 1024);
        const auto bytes = catalog.properties.dump().size() + catalog.extensions.dump().size() + catalog.id.size() + 64;
        if (bytes > 4 * 1024 * 1024 - budget.encoded_bytes) reject("group catalog byte limit exceeded");
        budget.encoded_bytes += bytes;
        const auto [found, inserted] = pooled.emplace(catalog.id, &catalog);
        if (!inserted && (*found->second != catalog || found->second->properties.dump() != catalog.properties.dump() ||
            found->second->extensions.dump() != catalog.extensions.dump()))
            reject("shared catalog identities require exact material definitions");
        const auto model = AssemblyModel::from_json(catalog.properties.at("model"));
        if (!model.types().empty() || !model.instances().empty()) reject("group catalogs may carry material definitions only");
    }
}

static void validate_transfer_group(const std::vector<const CornerWindowTransfer*>& transfers,
    std::size_t values = 0, std::size_t text_bytes = 0, std::size_t encoded_bytes = 0) {
    if (transfers.empty() || transfers.size() > 128) reject("group member limit exceeded");
    std::size_t dimensions = 0;
    for (const auto* pointer : transfers) {
        const auto& transfer = *pointer;
        if (transfer.dimensions.size() > dimension_row_limit - dimensions)
            reject("group dimension limit exceeded");
        dimensions += transfer.dimensions.size();
    }
    if (3 * transfers.size() + dimensions > 4096) reject("group change limit exceeded");
    constexpr std::size_t group_bytes = 4 * 1024 * 1024;
    Ids owned;
    std::map<std::string, const Entity*, std::less<>> hosts;
    const auto inspect = [&](const Entity& entity) {
        validate_json(entity.properties, 0, values, text_bytes, group_bytes);
        validate_json(entity.extensions, 0, values, text_bytes, group_bytes);
        const auto bytes = entity.properties.dump().size() + entity.extensions.dump().size() +
            entity.id.size() + entity.type.size() + 64;
        if (bytes > group_bytes - encoded_bytes) reject("group encoded byte limit exceeded");
        encoded_bytes += bytes;
    };
    for (const auto* pointer : transfers) {
        const auto& transfer = *pointer;
        const auto own = [&](const Entity& entity) {
            inspect(entity);
            if (!owned.insert(entity.id).second || hosts.contains(entity.id))
                reject("group owner, cut and dimension identities must be distinct");
        };
        own(transfer.owner);
        for (const auto& cut : transfer.cuts) own(cut);
        for (const auto& dimension : transfer.dimensions) own(dimension);
        for (const auto& host : transfer.walls) {
            inspect(host);
            if (owned.contains(host.id)) reject("group host identity overlaps transported content");
            const auto [found, inserted] = hosts.emplace(host.id, &host);
            if (!inserted && (*found->second != host ||
                found->second->properties.dump() != host.properties.dump() ||
                found->second->extensions.dump() != host.extensions.dump()))
                reject("shared group hosts require exact source envelopes");
        }
    }
    for (const auto* transfer : transfers) validate_corner_window_transfer(*transfer);
}

void validate_corner_window_transfer_group(const std::vector<CornerWindowTransfer>& transfers,
    const std::vector<Entity>& material_catalogs) {
    if (transfers.empty() || transfers.size() > 128) reject("group member limit exceeded");
    GroupBudget budget;
    validate_material_catalog_group(material_catalogs, budget);
    std::vector<const CornerWindowTransfer*> passive;
    for (const auto& transfer : transfers) passive.push_back(&transfer);
    validate_transfer_group(passive, budget.values, budget.text_bytes, budget.encoded_bytes);
    for (const auto& catalog : material_catalogs) for (const auto& transfer : transfers) {
        if (catalog.id == transfer.owner.id) reject("material catalog identity overlaps a source owner");
        for (const auto& wall : transfer.walls) if (catalog.id == wall.id) reject("material catalog identity overlaps a source host");
        for (const auto& cut : transfer.cuts) if (catalog.id == cut.id) reject("material catalog identity overlaps a source cut");
        for (const auto& dimension : transfer.dimensions) if (catalog.id == dimension.id) reject("material catalog identity overlaps a source dimension");
    }
}

ApplyEntityChanges corner_window_group_clone_command(const DocumentSnapshot& destination,
    const std::vector<CornerWindowCloneRequest>& requests, Revision expected_revision,
    const std::vector<Entity>& imported_material_catalogs, const std::vector<Entity>& fresh_wall_hosts) {
    if (requests.empty() || requests.size() > 128) reject("group request limit exceeded");
    if (!destination.is_editable() || destination.revision() != expected_revision)
        reject("destination is read-only or stale");
    if (imported_material_catalogs.size() > 128) reject("group catalog limit exceeded");
    std::size_t dimensions = 0;
    for (const auto& request : requests) {
        if (request.transfer.dimensions.size() > dimension_row_limit - dimensions)
            reject("group dimension limit exceeded");
        dimensions += request.transfer.dimensions.size();
        if (request.dimension_ids.size() != request.transfer.dimensions.size())
            reject("group dimension identity map must exactly cover its member");
    }
    if (requests.size() * 3 + dimensions + imported_material_catalogs.size() > 4096)
        reject("group change limit exceeded");
    GroupBudget budget;
    validate_material_catalog_group(imported_material_catalogs, budget);
    Ids fresh;
    std::map<std::string, Entity, std::less<>> prepared_hosts;
    if (fresh_wall_hosts.size() > 128) reject("fresh wall host limit exceeded");
    for (const auto& host : fresh_wall_hosts) {
        validate_envelope(host, "wall");
        validate_json(host.properties, 0, budget.values, budget.text_bytes, 4 * 1024 * 1024);
        validate_json(host.extensions, 0, budget.values, budget.text_bytes, 4 * 1024 * 1024);
        const auto bytes = host.properties.dump().size() + host.extensions.dump().size() + host.id.size() + 64;
        if (bytes > 4 * 1024 * 1024 - budget.encoded_bytes) reject("fresh wall host byte limit exceeded");
        budget.encoded_bytes += bytes;
        if (destination.entities().contains(host.id) || !fresh.insert(host.id).second)
            reject("prepared wall hosts require distinct fresh identities");
        prepared_hosts.emplace(host.id, host);
    }
    std::vector<const CornerWindowTransfer*> passive;
    for (const auto& request : requests) passive.push_back(&request.transfer);
    validate_transfer_group(passive, budget.values, budget.text_bytes, budget.encoded_bytes);
    for (const auto& request : requests) {
        const auto add = [&](const std::string& id) {
            if (!valid_id(id) || !fresh.insert(id).second) reject("group clone identities must be distinct and fresh");
        };
        add(request.owner_id);
        for (const auto& id : request.opening_ids) add(id);
        if (request.dimension_ids.size() != request.transfer.dimensions.size())
            reject("group dimension identity map must exactly cover its member");
        for (const auto& [original, id] : request.dimension_ids) { (void)original; add(id); }
    }
    reserve_identities(destination, passive, fresh, imported_material_catalogs);
    ApplyEntityChanges result{expected_revision, {}, {}, requests.size() == 1 ? "Clone corner window" : "Clone corner windows"};
    for (const auto& request : requests) {
        auto member = clone_command(destination, request.transfer, request.owner_id, request.opening_ids,
            request.wall_ids, request.at_start, expected_revision, request.dimension_ids, false, &prepared_hosts);
        if (member.entity_changes.size() > 4096 - result.entity_changes.size()) reject("group change limit exceeded");
        for (auto& change : member.entity_changes) result.entity_changes.push_back(std::move(change));
    }
    for (const auto& catalog : imported_material_catalogs) result.entity_changes.push_back(EntityChange::upsert(catalog));
    return result;
}
} // namespace sketch
