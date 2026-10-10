#include "sketch/corner_window_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/document_wall.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr double tolerance = default_geometry_tolerance_metres;
constexpr std::array owner_pointers{
    "/widths_m/0", "/widths_m/1", "/sill_m", "/height_m",
    "/opening_assembly/frame_width_m", "/opening_assembly/frame_depth_m",
    "/opening_assembly/panel_thickness_m", "/opening_assembly/glazing_thickness_m",
    "/opening_assembly/inset_m"};
constexpr std::array cut_pointers{
    "/offset_m", "/offset", "/width_m", "/width", "/sill_m", "/sill", "/height_m", "/height"};
constexpr std::array context_keys{"property_id", "building_id", "floor_id", "layer_id", "level_id"};

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Corner window edit: " + reason);
}

// Ordinary json equality intentionally equates several numeric storage types.
// Opaque payloads and change filtering must retain their exact representation.
bool same_json(const Json& left, const Json& right) {
    if (left.type() != right.type()) return false;
    if (left.is_array()) {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
            if (!same_json(left[i], right[i])) return false;
        return true;
    }
    if (left.is_object()) {
        if (left.size() != right.size()) return false;
        for (const auto& [key, value] : left.items()) {
            const auto found = right.find(key);
            if (found == right.end() || !same_json(value, *found)) return false;
        }
        return true;
    }
    if (left.is_number_float()) {
        const double a = left.get<double>(), b = right.get<double>();
        return a == b && (a != 0.0 || std::signbit(a) == std::signbit(b));
    }
    return left == right;
}

bool same_entity(const Entity& left, const Entity& right) {
    return left.id == right.id && left.type == right.type && left.required == right.required &&
        same_json(left.properties, right.properties) && same_json(left.extensions, right.extensions);
}

void validate_json(const Json& value, std::size_t depth, std::size_t& nodes, std::size_t& bytes) {
    constexpr std::size_t byte_limit = 1024 * 1024;
    if (depth > 64 || ++nodes > 100'000) reject("payload exceeds complexity limits");
    if (value.is_binary() || value.is_discarded() ||
        (value.is_number_float() && !std::isfinite(value.get<double>())))
        reject("payload contains a nonportable value");
    const auto add = [&](std::size_t count) {
        if (count > byte_limit - bytes) reject("payload exceeds byte limits");
        bytes += count;
    };
    if (value.is_string()) add(value.get_ref<const std::string&>().size());
    else if (value.is_array())
        for (const auto& child : value) validate_json(child, depth + 1, nodes, bytes);
    else if (value.is_object())
        for (const auto& [key, child] : value.items()) {
            if (key.size() > 128) reject("payload contains an oversized key");
            add(key.size());
            validate_json(child, depth + 1, nodes, bytes);
        }
}

void validate_envelope(const Entity& entity) {
    for (const auto* object : {&entity.properties, &entity.extensions}) {
        if (!object->is_object()) reject("properties and extensions must be objects");
        std::size_t nodes = 0, bytes = 0;
        validate_json(*object, 0, nodes, bytes);
        try {
            if (object->dump().size() > 1024 * 1024) reject("encoded payload exceeds byte limits");
        } catch (const Json::exception&) { reject("payload cannot be encoded"); }
    }
    for (const auto* key : {"id", "type", "required", "properties"})
        if (entity.extensions.contains(key)) reject("extensions shadow reserved fields");
    const auto entries = entity.properties.find("quantity_entries");
    if (entries != entity.properties.end() && !entries->is_object())
        reject("quantity_entries must be an object");
}

const Entity& target(const Entities& entities, const std::string& id, std::string_view type) {
    const auto found = entities.find(id);
    if (found == entities.end() || found->second.id != id || found->second.type != type)
        reject("missing or wrong-type participant " + id);
    return found->second;
}

const Json& optional(const Json& properties, const char* key) {
    static const Json absent;
    const auto found = properties.find(key);
    return found == properties.end() ? absent : *found;
}

void require_context(const Entity& entity, const Entity& host) {
    for (const auto* key : context_keys)
        if (!same_json(optional(entity.properties, key), optional(host.properties, key)) ||
            (entity.properties.contains(key) && !entity.properties.at(key).is_string()))
            reject("raw drawing context and level bindings must remain common");
}

std::array<Wall, 2> hosts(const Entities& entities, const CornerWindow& owner) {
    std::array<Wall, 2> result;
    const auto& first = target(entities, owner.wall_ids[0], "wall");
    for (std::size_t leg = 0; leg < 2; ++leg) {
        const auto& entity = target(entities, owner.wall_ids[leg], "wall");
        validate_envelope(entity);
        require_context(entity, first);
        if (!same_json(optional(entity.properties, "vertical_placement"),
                       optional(first.properties, "vertical_placement")))
            reject("hosts require identical raw vertical placement");
        std::string error;
        if (!read_document_wall(entity, {}, result[leg], error)) reject(error);
        validate_wall_semantics(result[leg]);
        if (result[leg].baseline.sweep_radians != 0.0) reject("only straight hosts are supported");
    }
    return result;
}

bool numeric_core(const Json& receipt) {
    const auto version = receipt.find("version");
    if (!receipt.is_object() || version == receipt.end() ||
        !version->is_number_integer() || *version != 1) return false;
    const auto exact = receipt.find("exact_metres");
    return receipt.contains("original_expression") || receipt.contains("entered_unit") ||
        (exact != receipt.end() && (!exact->is_object() || exact->contains("numerator") || exact->contains("denominator")));
}

Quantity decode_receipt(const Json& receipt) {
    if (!receipt.is_object() || !receipt.contains("original_expression") ||
        !receipt.at("original_expression").is_string() ||
        receipt.at("original_expression").get_ref<const std::string&>().size() > 4096)
        reject("quantity receipt lacks a supported bounded expression");
    try { return decode_constraint_quantity_receipt(receipt); }
    catch (const std::exception&) { reject("quantity receipt has an unsupported numeric core"); }
}

template<std::size_t N>
void validate_receipts(const Entity& entity, const std::array<const char*, N>& pointers) {
    const auto entries = entity.properties.find("quantity_entries");
    if (entries == entity.properties.end()) return;
    for (const auto* pointer : pointers) {
        const auto receipt = entries->find(pointer);
        if (receipt == entries->end() || !numeric_core(*receipt)) continue;
        const Json::json_pointer path(pointer);
        if (!entity.properties.contains(path) || !entity.properties.at(path).is_number() ||
            decode_receipt(*receipt).metres != entity.properties.at(path).get<double>())
            reject("current quantity receipt is stale against its source scalar");
    }
}

Json numeric_receipt(double metres) {
    std::array<char, 64> buffer{};
    for (int precision = 17; precision > 0; --precision) {
        const auto encoded = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
            metres, std::chars_format::general, precision);
        if (encoded.ec != std::errc{}) continue;
        try {
            const auto value = parse_quantity(std::string(buffer.data(), encoded.ptr) + " m", Unit::metre);
            if (value.metres != metres) continue;
            Json result{{"version", 1}, {"original_expression", value.original_expression},
                {"entered_unit", "m"}, {"exact_metres", {{"numerator", value.exact_metres.numerator},
                                                       {"denominator", value.exact_metres.denominator}}}};
            if (decode_constraint_quantity_receipt(result).metres == metres) return result;
        } catch (const std::invalid_argument&) { }
          catch (const std::overflow_error&) { }
    }
    reject("changed dimension has no exact bounded metre receipt");
}

template<std::size_t N>
void replay_receipts(const Entity& before, Entity& after, const std::array<const char*, N>& pointers) {
    const auto entries = before.properties.find("quantity_entries");
    if (entries == before.properties.end()) return;
    Json retained = *entries;
    for (const auto* pointer : pointers) {
        const Json::json_pointer path(pointer);
        const auto found = retained.find(pointer);
        if (found == retained.end()) continue;
        if (!before.properties.contains(path)) {
            if (after.properties.contains(path)) reject("receipt has no source scalar binding");
            continue;
        }
        if (!after.properties.contains(path) || !after.properties.at(path).is_number())
            reject("receipt binding lost its destination scalar");
        if (before.properties.at(path).get<double>() == after.properties.at(path).get<double>()) continue;
        auto& receipt = *found;
        if (!numeric_core(receipt) || decode_receipt(receipt).metres != before.properties.at(path).get<double>())
            reject("changed quantity binding requires a current matching numeric core");
        const auto encoded = numeric_receipt(after.properties.at(path).get<double>());
        for (const auto* key : {"version", "original_expression", "entered_unit"}) receipt[key] = encoded.at(key);
        for (const auto* key : {"numerator", "denominator"})
            receipt["exact_metres"][key] = encoded.at("exact_metres").at(key);
    }
    after.properties["quantity_entries"] = std::move(retained);
}

template<std::size_t N>
void retain_unchanged_numbers(const Entity& before, Entity& after, const std::array<const char*, N>& pointers) {
    for (const auto* pointer : pointers) {
        const Json::json_pointer path(pointer);
        if (before.properties.contains(path) && after.properties.contains(path) &&
            before.properties.at(path).is_number() && after.properties.at(path).is_number() &&
            before.properties.at(path).get<double>() == after.properties.at(path).get<double>())
            after.properties[path] = before.properties.at(path);
    }
}

void retain_cut_dimension(Json& properties, const char* canonical, const char* alias, double value) {
    bool present = false;
    for (const auto* key : {canonical, alias}) {
        const auto found = properties.find(key);
        if (found == properties.end()) continue;
        present = true;
        if (found->get<double>() != value) *found = value;
    }
    if (!present) reject(std::string("source cut lacks ") + canonical);
}

template<std::size_t N>
void require_receipt_retention(const Entity& source, const Entity& candidate,
    const std::array<const char*, N>& pointers) {
    const auto& original = optional(source.properties, "quantity_entries");
    const auto& staged = optional(candidate.properties, "quantity_entries");
    if (same_json(original, staged)) return;
    // A completion may run again after a later host consequence. Only our
    // exact source-derived replay is recognized, including every opaque sibling.
    if (!source.properties.contains("quantity_entries"))
        reject("candidate introduced unrelated receipt payload");
    Entity expected = candidate;
    replay_receipts(source, expected, pointers);
    if (!same_json(staged, optional(expected.properties, "quantity_entries")))
        reject("candidate replaced source-owned receipt metadata or numeric core");
}

void require_cut_payload(const Entity& source, const Entity& candidate) {
    auto original = source.properties;
    auto staged = candidate.properties;
    for (const auto* pointer : cut_pointers) {
        const Json::json_pointer path(pointer);
        if (original.contains(path) != staged.contains(path)) reject("cut scalar aliases must remain retained");
        if (original.contains(path)) {
            if (!staged.at(path).is_number() || !std::isfinite(staged.at(path).get<double>()))
                reject("candidate cut dimensions must be finite numbers");
            original[path] = nullptr; staged[path] = nullptr;
        }
    }
    require_receipt_retention(source, candidate, cut_pointers);
    original.erase("quantity_entries"); staged.erase("quantity_entries");
    if (source.required != candidate.required || !same_json(source.extensions, candidate.extensions) ||
        !same_json(original, staged)) reject("completion cannot replace unrelated cut payloads");
}

void require_owner_retention(const Entity& source, const Entity& candidate) {
    if (source.required != candidate.required || !same_json(source.extensions, candidate.extensions))
        reject("owner envelope must retain its source metadata");
    for (const auto* key : context_keys)
        if (!same_json(optional(source.properties, key), optional(candidate.properties, key)))
            reject("owner context remapping is outside geometry completion");
    require_receipt_retention(source, candidate, owner_pointers);
    // Property edits may author ordinary fields. Unrecognized retained content
    // cannot disappear merely because a known assembly was serialized anew.
    for (const auto& [key, value] : source.properties.items()) {
        if (key == "opening_assembly" || key == "widths_m" || key == "sill_m" || key == "height_m" ||
            key == "name" || key == "quantity_entries") continue;
        if (!candidate.properties.contains(key) || !same_json(value, candidate.properties.at(key)))
            reject("owner edit changed retained metadata " + key);
    }
    auto before = source.properties.at("opening_assembly");
    auto after = candidate.properties.at("opening_assembly");
    for (const auto* pointer : owner_pointers) {
        const std::string_view text(pointer);
        constexpr std::string_view prefix = "/opening_assembly/";
        if (!text.starts_with(prefix)) continue;
        const std::string field(text.substr(prefix.size()));
        before.erase(field); after.erase(field);
    }
    if (!same_json(before, after)) reject("assembly edit changed retained non-dimensional profile payload");
}

void validate_topology(const Entities& source, const Entities& entities) {
    std::map<std::string, std::pair<std::string, std::size_t>, std::less<>> children;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "corner_window" || !source.contains(id)) continue;
        const auto owner = parse_corner_window(entity);
        for (std::size_t leg = 0; leg < 2; ++leg)
            if (!children.emplace(owner.opening_ids[leg], std::pair{id, leg}).second)
                reject("cut has overlapping owner topology");
    }
    for (const auto& [id, entity] : entities) {
        if (!entity.properties.is_object()) continue;
        const bool backlink = entity.properties.contains("corner_window_id");
        const bool indexed = entity.properties.contains("corner_leg");
        const auto found = children.find(id);
        if (!backlink && !indexed && found == children.end()) continue;
        // A fresh aggregate is completed by its own producer and admitted by
        // the caller. Its topology cannot become authority for an existing one.
        if (backlink && entity.properties.at("corner_window_id").is_string()) {
            const auto owner_id = entity.properties.at("corner_window_id").get<std::string>();
            const auto owner = entities.find(owner_id);
            if (!source.contains(owner_id) && owner != entities.end() && owner->second.type == "corner_window") continue;
        }
        if (entity.type != "opening" || !backlink || !indexed || found == children.end() ||
            entity.properties.at("corner_window_id") != found->second.first ||
            !entity.properties.at("corner_leg").is_number_integer() ||
            entity.properties.at("corner_leg") != found->second.second)
            reject("orphan or inconsistent cut topology");
    }
}

void require_scaled_host(const Wall& before, const Wall& after, double factor) {
    const auto length = [](const Wall& value) {
        return std::hypot(value.baseline.end.x - value.baseline.start.x,
                          value.baseline.end.y - value.baseline.start.y);
    };
    const auto matches = [&](double source, double candidate) {
        const double expected = source * factor;
        return std::isfinite(expected) && std::abs(candidate - expected) <=
            std::max(tolerance, 32.0 * std::numeric_limits<double>::epsilon() * std::abs(expected));
    };
    if (!matches(length(before), length(after)) || !matches(before.thickness, after.thickness) ||
        !matches(before.height, after.height)) reject("declared scale differs from actual host dimensions");
}
} // namespace

void complete_corner_window_geometry(const Entities& source, Entities& candidate,
    const std::map<std::string, double, std::less<>>& wall_scales) {
    for (const auto& [id, factor] : wall_scales) {
        (void)target(source, id, "wall");
        if (!std::isfinite(factor) || factor <= 0.0) reject("wall scale must be finite and positive");
    }
    // Bound every source participant before the full exact source validator.
    for (const auto& [id, entity] : source) {
        if (entity.type != "corner_window") continue;
        validate_envelope(entity);
        const auto owner = parse_corner_window(entity);
        for (const auto& wall : owner.wall_ids) validate_envelope(target(source, wall, "wall"));
        for (const auto& cut : owner.opening_ids) validate_envelope(target(source, cut, "opening"));
        const bool retained_owner = candidate.contains(id);
        const bool retained_cut0 = candidate.contains(owner.opening_ids[0]);
        const bool retained_cut1 = candidate.contains(owner.opening_ids[1]);
        if (!retained_owner && !retained_cut0 && !retained_cut1) continue;
        if (!retained_owner || !retained_cut0 || !retained_cut1)
            reject("existing aggregate participant inventory is partially removed");
        validate_envelope(target(candidate, id, "corner_window"));
        for (const auto& wall : owner.wall_ids) validate_envelope(target(candidate, wall, "wall"));
        for (const auto& cut : owner.opening_ids) validate_envelope(target(candidate, cut, "opening"));
    }
    validate_corner_window_state(source);
    validate_topology(source, candidate);
    std::vector<Entity> replacements;
    for (const auto& [id, entity] : source) {
        if (entity.type != "corner_window") continue;
        const auto before = parse_corner_window(entity);
        if (!candidate.contains(id)) continue; // The complete trio was removed above.
        const auto& staged = target(candidate, id, "corner_window");
        validate_envelope(staged);
        validate_receipts(entity, owner_pointers);
        auto after = parse_corner_window(staged);
        if (after.wall_ids != before.wall_ids || after.opening_ids != before.opening_ids ||
            after.at_start != before.at_start) reject("host/child identities and leg endpoints must remain fixed");
        require_owner_retention(entity, staged);
        const auto old_hosts = hosts(source, before);
        const auto new_hosts = hosts(candidate, after);
        const auto scale0 = wall_scales.find(before.wall_ids[0]);
        const auto scale1 = wall_scales.find(before.wall_ids[1]);
        Entity owner = staged;
        if (scale0 != wall_scales.end() || scale1 != wall_scales.end()) {
            if (scale0 == wall_scales.end() || scale1 == wall_scales.end() || scale0->second != scale1->second)
                reject("both corner hosts require the same declared scale");
            if (after != before) reject("host scaling cannot combine with an independent owner dimension edit");
            for (std::size_t leg = 0; leg < 2; ++leg) require_scaled_host(old_hosts[leg], new_hosts[leg], scale0->second);
            for (const auto* pointer : owner_pointers) {
                const Json::json_pointer path(pointer);
                const double value = entity.properties.at(path).get<double>() * scale0->second;
                if (!std::isfinite(value)) reject("scaled owner dimension is nonfinite");
                if (value != entity.properties.at(path).get<double>()) owner.properties[path] = value;
            }
            after = parse_corner_window(owner);
        }
        require_context(owner, target(candidate, after.wall_ids[0], "wall"));
        if (owner.properties.contains("vertical_placement")) reject("owner elevation must remain host-owned");
        for (const auto& wall : new_hosts)
            if (after.assembly.frame_depth_m > wall.thickness + tolerance ||
                std::abs(after.assembly.inset_m) + after.assembly.frame_depth_m * 0.5 > wall.thickness * 0.5 + tolerance)
                reject("profile does not fit both host thicknesses");
        const auto cuts = corner_window_cuts(after, new_hosts);
        retain_unchanged_numbers(entity, owner, owner_pointers);
        replay_receipts(entity, owner, owner_pointers);
        validate_envelope(owner);
        if (!same_entity(staged, owner)) replacements.push_back(std::move(owner));
        for (std::size_t leg = 0; leg < 2; ++leg) {
            const auto& source_cut = target(source, before.opening_ids[leg], "opening");
            const auto& candidate_cut = target(candidate, before.opening_ids[leg], "opening");
            validate_envelope(candidate_cut);
            validate_receipts(source_cut, cut_pointers);
            require_cut_payload(source_cut, candidate_cut);
            Entity cut = source_cut;
            retain_cut_dimension(cut.properties, "offset_m", "offset", cuts[leg].offset);
            retain_cut_dimension(cut.properties, "width_m", "width", cuts[leg].width);
            retain_cut_dimension(cut.properties, "sill_m", "sill", cuts[leg].sill);
            retain_cut_dimension(cut.properties, "height_m", "height", cuts[leg].height);
            replay_receipts(source_cut, cut, cut_pointers);
            validate_envelope(cut);
            // Restore an unchanged known scalar's representation even if an
            // earlier generic producer rewrote it while staging the wall.
            if (!same_entity(candidate_cut, cut)) replacements.push_back(std::move(cut));
        }
    }
    for (auto& entity : replacements) candidate.at(entity.id) = std::move(entity);
}

ApplyEntityChanges corner_window_leg_resize_command(const DocumentSnapshot& source,
    const std::string& owner_id, std::size_t leg, double relative_width_scale) {
    if (!source.is_editable()) reject("source is read-only");
    if (leg >= 2 || !std::isfinite(relative_width_scale) || relative_width_scale <= 0.0)
        reject("leg and relative scale must be valid");
    const auto& owner = target(source.entities(), owner_id, "corner_window");
    const auto corner = parse_corner_window(owner);
    const double width = corner.widths[leg] * relative_width_scale;
    if (!std::isfinite(width)) reject("resized leg width is nonfinite");
    auto candidate = source.entities();
    if (width != corner.widths[leg]) candidate.at(owner_id).properties["widths_m"][leg] = width;
    complete_corner_window_geometry(source.entities(), candidate);
    ApplyEntityChanges command{source.revision(), {}, {}, "Resize corner window leg"};
    for (const auto& id : {owner_id, corner.opening_ids[0], corner.opening_ids[1]})
        if (!same_entity(source.entities().at(id), candidate.at(id)))
            command.entity_changes.push_back(EntityChange::upsert(std::move(candidate.at(id))));
    return command;
}

} // namespace sketch
