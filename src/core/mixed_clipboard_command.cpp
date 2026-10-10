#include "sketch/mixed_clipboard_command.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/authored_phase_membership.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/stair_semantics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t value_limit = 100000;
constexpr std::size_t depth_limit = 64;
constexpr std::size_t change_limit = 4096;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Mixed clipboard placement: ") + reason);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
void identity(std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) reject("invalid identity");
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source binding must be a digest");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("source binding must be canonical SHA-256");
}
struct Budget {
    std::size_t bytes{}, values{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("encoded byte budget exceeded");
        bytes += count;
    }
    void node(std::size_t depth) {
        if (depth > depth_limit || ++values > value_limit) reject("JSON complexity budget exceeded");
    }
    void quoted(std::string_view value) {
        add(2);
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c >= 0x80) {
                const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!tail || tail >= value.size() - i) reject("invalid UTF-8 string");
                const auto first = static_cast<unsigned char>(value[i + 1]);
                if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                    (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) reject("invalid UTF-8 string");
                for (std::size_t j = 1; j <= tail; ++j) {
                    const auto byte = static_cast<unsigned char>(value[i + j]);
                    if (byte < 0x80 || byte > 0xbf) reject("invalid UTF-8 string");
                }
                add(tail + 1); i += tail;
                continue;
            }
            if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') add(2);
            else add(c < 0x20 ? 6 : 1);
        }
    }
    void string(std::string_view value, std::size_t depth) { node(depth); quoted(value); }
    void object(std::initializer_list<const char*> names, std::size_t depth) {
        node(depth); add(2 + (names.size() ? names.size() - 1 : 0));
        for (const auto* name : names) { quoted(name); add(1); }
    }
    void array(std::size_t count, std::size_t depth) {
        node(depth); add(2 + (count ? count - 1 : 0));
    }
    void json(const Json& value, std::size_t depth) {
        node(depth);
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else if (value.is_object()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); json(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& child : value) json(child, depth + 1);
        } else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
    void entity(const Entity& value, std::size_t depth) {
        object({"id", "type", "properties", "required", "extensions"}, depth);
        string(value.id, depth + 1); string(value.type, depth + 1);
        json(value.properties, depth + 1); node(depth + 1); add(5);
        json(value.extensions, depth + 1);
    }
};

bool placeable(std::string_view type) {
    // Physical clipboard roots, their catalog/relationship/annotation children.
    // No hierarchy, phase registry, sheet/view graph or import registry owner.
    constexpr std::array<std::string_view, 23> allowed{
        "boundary", "measurement_boundary", "measurement_linework", "room_boundary",
        "wall", "opening", "corner_window", "room", "slab", "roof", "stair",
        "railing", "column", "beam", "terrain_surface", "annotation_state",
        "assembly_instance", "assembly_model", "dimension", "constraint", "label",
        "wall_join", "roof_join"};
    return std::find(allowed.begin(), allowed.end(), type) != allowed.end();
}
void additions_shape(const ApplyEntityChanges& additions) {
    if (additions.entity_changes.empty() || additions.entity_changes.size() > change_limit ||
        !additions.asset_changes.empty()) reject("requires one to 4096 fresh changes without assets");
    Ids unique;
    for (const auto& change : additions.entity_changes) {
        if (change.kind != EntityChangeKind::upsert ||
            (!change.entity_id.empty() && change.entity_id != change.entity.id))
            reject("additions permit canonical fresh upserts only");
        identity(change.entity.id);
        if (!placeable(change.entity.type)) reject("addition type has no clipboard placement authority");
        if (!unique.insert(change.entity.id).second) reject("duplicate added owner");
    }
}
ApplyEntityChanges decode_additions(const Json& value, Revision expected) {
    fields(value, {"version", "kind", "expected_revision", "message", "entity_changes", "asset_changes"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
        value.at("kind") != "apply_entity_changes" || !value.at("entity_changes").is_array() ||
        value.at("entity_changes").empty() || value.at("entity_changes").size() > change_limit ||
        !value.at("asset_changes").is_array() || !value.at("asset_changes").empty())
        reject("unsupported fresh additions command");
    if (revision(value.at("expected_revision")) != expected) reject("additions have a different source revision");
    for (const auto& row : value.at("entity_changes")) {
        fields(row, {"kind", "entity"});
        if (row.at("kind") != "upsert") reject("additions cannot erase entities");
    }
    auto decoded = command_from_json(value);
    auto additions = std::get<ApplyEntityChanges>(std::move(decoded));
    additions_shape(additions);
    if (command_to_json(Command{additions}).dump() != value.dump()) reject("additions are not canonical");
    return additions;
}
void roof_shape(const Json& roof, const Json& enclosing) {
    if (roof.is_null()) return;
    if (!roof.is_object() || !roof.contains("version") ||
        !roof.at("version").is_number_integer() ||
        (roof.at("version") != 4 && roof.at("version") != 17 &&
         roof.at("version") != 20 && roof.at("version") != 21))
        reject("roof authority requires a standalone roof phase dialect");
    const auto decoded = decode_phase_constraint_authoring_intent(roof);
    if (!decoded.roof_replacement.is_null() &&
        decode_phase_roof_replacement_authoring(decoded.roof_replacement).demolition)
        reject("clipboard roof replacement cannot confer demolition authority");
    if (encode_phase_constraint_authoring_intent(decoded).dump() != roof.dump())
        reject("roof authority is not canonical");
    for (const auto* key : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision"})
        if (roof.at(key).dump() != enclosing.at(key).dump())
            reject("roof and placement must bind the same original source");
    auto empty = decoded;
    empty.intent = ConstraintAuthoringIntent{};
    empty.intent.message = decoded.intent.message;
    if (encode_phase_constraint_authoring_intent(empty).dump() != roof.dump())
        reject("roof authority cannot borrow ordinary constraint authoring");
}
void validate(const Json& value) {
    Budget budget; budget.json(value, 0); // Before copies, dump or family codecs.
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "roof_authoring", "additions",
        "selected_registry_id", "message"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        reject("unsupported intent version");
    const auto expected = revision(value.at("expected_revision"));
    if (expected == std::numeric_limits<Revision>::max()) reject("source revision cannot advance");
    for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"})
        digest(value.at(key));
    if (!value.at("source_saved_revision").is_null() &&
        revision(value.at("source_saved_revision")) > expected)
        reject("saved revision exceeds captured source");
    if (!value.at("selected_registry_id").is_string()) reject("destination registry must be a string");
    const auto& registry = value.at("selected_registry_id").get_ref<const std::string&>();
    if (!registry.empty()) identity(registry);
    if (!value.at("message").is_string() || value.at("message").get_ref<const std::string&>().size() > 1024 ||
        value.at("message").get_ref<const std::string&>().find('\0') != std::string::npos)
        reject("invalid placement message");
    (void)decode_additions(value.at("additions"), expected);
    roof_shape(value.at("roof_authoring"), value);
}

// A page registration changes only the saved view's explicit object roster.
// Actual source assets are independently checked at snapshot admission.
struct Page {
    std::string view, sheet, floor, calculation, interior;
};
std::vector<Page> pages(const Entities& actual) {
    for (const auto& [id, entity] : actual) {
        (void)id;
        if (entity.type != kSheetViewEntityType || !entity.extensions.contains("pinc_import")) continue;
        const auto& metadata = entity.extensions.at("pinc_import");
        if (!metadata.is_object() || metadata.value("version", 0) != 1 ||
            !metadata.contains("pages") || !metadata.at("pages").is_array() ||
            metadata.at("pages").size() > 128 || !metadata.contains("source_asset_id") ||
            !metadata.at("source_asset_id").is_string()) reject("invalid imported page metadata");
        const auto model = decode_sheet_view_entity(entity);
        const auto organization = organize_project(actual);
        std::vector<Page> result;
        Ids layers;
        for (const auto& raw : metadata.at("pages")) {
            if (!raw.is_object()) reject("invalid imported page record");
            const auto text = [&](const char* key) {
                if (!raw.contains(key) || !raw.at(key).is_string() ||
                    raw.at(key).get_ref<const std::string&>().size() > 4096)
                    reject("invalid imported page field");
                return raw.at(key).get<std::string>();
            };
            (void)text("name");
            Page page{text("view_id"), text("sheet_id"), text("floor_id"),
                text("calculation_layer_id"), text("interior_layer_id")};
            if (!raw.contains("ghost_previous") || !raw.at("ghost_previous").is_boolean() ||
                !raw.contains("show_print_guide") || !raw.at("show_print_guide").is_boolean() ||
                !raw.contains("page_index") || !raw.at("page_index").is_number_unsigned())
                reject("invalid imported page flags/index");
            const auto calculation = organization.drawing_context(page.calculation);
            const auto interior = organization.drawing_context(page.interior);
            if (!calculation || !interior ||
                std::none_of(model.views().begin(), model.views().end(), [&](const auto& view) { return view.id == page.view; }) ||
                std::none_of(model.sheets().begin(), model.sheets().end(), [&](const auto& sheet) { return sheet.id == page.sheet; }))
                continue; // Deliberately removed imported containers remain retired.
            if (raw.at("page_index").get<std::size_t>() >= 128 ||
                calculation->floor_id != page.floor || interior->floor_id != page.floor ||
                !layers.insert(page.calculation).second || !layers.insert(page.interior).second)
                reject("inconsistent imported page links");
            result.push_back(std::move(page));
        }
        return result;
    }
    return {};
}
void enroll_pages(const Entities& actual, Entities& candidate, const ApplyEntityChanges& fresh) {
    const auto records = pages(actual);
    if (records.empty()) return;
    for (const auto& [id, original] : actual) {
        if (original.type != kSheetViewEntityType || !original.extensions.contains("pinc_import")) continue;
        auto& updated = candidate.at(id);
        auto& views = updated.properties.at("model").at("views");
        for (auto& view : views) {
            const auto page = std::find_if(records.begin(), records.end(), [&](const auto& record) {
                return record.view == view.at("id").get<std::string>();
            });
            if (page == records.end()) continue;
            if (!view.contains("object_ids")) {
                const bool enrolled = std::any_of(fresh.entity_changes.begin(), fresh.entity_changes.end(), [&](const auto& change) {
                    const auto layer = change.entity.properties.value("layer_id", std::string{});
                    return layer == page->calculation || layer == page->interior;
                });
                if (!enrolled) continue;
                view["object_ids"] = Json::array();
            }
            auto& objects = view.at("object_ids");
            for (const auto& change : fresh.entity_changes) {
                const auto layer = change.entity.properties.value("layer_id", std::string{});
                if (layer != page->calculation && layer != page->interior) continue;
                if (std::find(objects.begin(), objects.end(), Json(change.entity.id)) == objects.end())
                    objects.push_back(change.entity.id);
            }
        }
        (void)decode_sheet_view_entity(updated);
    }
}

// Only owned current child slots are fresh. Opaque/history, profile/material
// definitions and reference IDs in the copied entity are never fresh authority.
Ids owned_children(const Entity& entity) {
    Ids result;
    const auto add = [&](const std::string& id) { identity(id); result.insert(id); };
    if (can_recognize_boundary_entity_type(entity.type) &&
        entity.properties.contains("boundary_model_version")) {
        for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
            add(edge.segment_id); add(edge.start_vertex_id); add(edge.end_vertex_id);
        }
    } else if (entity.type == "measurement_linework") {
        const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
        if (!decoded.supported()) reject("unsupported measured addition identities");
        for (const auto& edge : decoded.model->edges) {
            add(edge.segment_id); add(edge.start_vertex_id); add(edge.end_vertex_id);
        }
    } else if (entity.type == "stair") {
        const auto stair = decode_stair_properties(entity.id, entity.properties);
        for (const auto& child : stair_child_ids(stair)) add(child);
    } else if (entity.type == "roof" && entity.properties.contains("roof_openings")) {
        for (const auto& row : entity.properties.at("roof_openings")) add(row.at("id").get<std::string>());
    } else if (entity.type == "annotation_state") {
        const auto& state = entity.properties.at("state");
        for (const auto* key : {"labels", "symbols"})
            for (const auto& row : state.at(key)) add(row.at("id").get<std::string>());
    }
    return result;
}
struct ReservedNames {
    Ids values;
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > 64 * 1024 * 1024 - bytes) reject("retained name string budget exceeded");
        bytes += value.size(); values.insert(value);
    }
    void json(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("retained name complexity budget exceeded");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); json(child, depth + 1);
        } else if (value.is_array()) for (const auto& child : value) json(child, depth + 1);
    }
    void entities(const Entities& actual) {
        for (const auto& [id, entity] : actual) {
            text(id); text(entity.type); json(entity.properties); json(entity.extensions);
        }
        for (const auto& [qualified, alias] : embedded_assembly_presentation_ids(actual)) {
            (void)qualified; text(alias);
        }
    }
};
Ids roof_new_names(const Entities& actual, const Entities& roof_stage) {
    Ids names;
    for (const auto& [id, entity] : roof_stage) {
        const auto original = actual.find(id);
        if (original == actual.end()) names.insert(id);
        // Replacement leaves can create owned dimensions/annotations as well
        // as roof children. Source-owned child IDs remain retained, not fresh.
        if (original != actual.end() && original->second == entity) continue;
        const auto children = owned_children(entity);
        const auto prior = original == actual.end() ? Ids{} : owned_children(original->second);
        for (const auto& child : children) if (!prior.contains(child)) names.insert(child);
    }
    const auto aliases = embedded_assembly_presentation_ids(roof_stage);
    const auto prior_aliases = embedded_assembly_presentation_ids(actual);
    for (const auto& [qualified, alias] : aliases)
        if (!prior_aliases.contains(qualified)) names.insert(alias);
    return names;
}
Entities roof_replay(const Entities& actual, const Json& value) {
    if (value.at("source_entities_digest") != entity_map_digest(actual))
        reject("proof does not bind the actual source entity map");
    const auto& registry_id = value.at("selected_registry_id").get_ref<const std::string&>();
    if (!registry_id.empty()) {
        const auto registry = actual.find(registry_id);
        if (registry == actual.end() || registry->second.type != "model_phases")
            reject("captured destination design set is absent from the actual source");
    }
    return value.at("roof_authoring").is_null() ? actual :
        replay_phase_constraint_authoring(actual, value.at("roof_authoring"));
}
void append_fresh(Entities& stage, const Entities& actual, const ApplyEntityChanges& additions) {
    const auto roof_names = roof_new_names(actual, stage);
    const auto prior_aliases = embedded_assembly_presentation_ids(stage);
    Ids fresh_names = roof_names;
    for (const auto& change : additions.entity_changes) {
        if (actual.contains(change.entity.id) || !fresh_names.insert(change.entity.id).second)
            reject("added owner collides with actual or roof-stage identity");
        for (const auto& child : owned_children(change.entity))
            if (!fresh_names.insert(child).second)
                reject("added child collides with another placement identity");
    }
    for (const auto& change : additions.entity_changes)
        if (!stage.emplace(change.entity.id, change.entity).second)
            reject("added owner collides with roof replay");
    const auto aliases = embedded_assembly_presentation_ids(stage);
    for (const auto& [qualified, alias] : prior_aliases) {
        const auto found = aliases.find(qualified);
        if (found == aliases.end() || found->second != alias)
            reject("fresh content changed an existing assembly presentation alias");
    }
}
} // namespace

Json validate_mixed_clipboard_placement_intent(const Json& value) {
    try { validate(value); return value; }
    catch (const Json::exception&) { reject("malformed intent"); }
}
bool mixed_clipboard_placement_active_phase_policy(const Json& value) {
    validate(value);
    return !value.at("roof_authoring").is_null() ||
        !value.at("selected_registry_id").get_ref<const std::string&>().empty();
}
std::optional<Revision> mixed_clipboard_placement_source_saved_revision(const Json& value) {
    validate(value);
    return value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
}
Entities replay_mixed_clipboard_placement(const Entities& actual, const Json& value) {
    validate(value);
    const auto roof_stage = roof_replay(actual, value);
    auto stage = roof_stage;
    auto additions = decode_additions(value.at("additions"), revision(value.at("expected_revision")));
    append_fresh(stage, actual, additions);
    // Freshness is relative to the independently replayed roof-only map.
    // Its explicit replacement cohort keeps its already-authored memberships.
    auto registration = additions;
    register_new_phase_memberships(roof_stage, registration, {},
        value.at("selected_registry_id").get<std::string>());
    for (const auto& change : registration.entity_changes)
        stage.insert_or_assign(change.entity.id, change.entity);
    enroll_pages(actual, stage, additions);
    return stage;
}
void validate_mixed_clipboard_placement_source(const DocumentSnapshot& source, const Json& value) {
    validate(value);
    if (!source.is_editable() || source.revision() != revision(value.at("expected_revision")) ||
        value.at("source_snapshot_digest") != document_snapshot_digest(source) ||
        value.at("source_authoring_digest") != document_authoring_source_digest_v2(source) ||
        value.at("source_entities_digest") != entity_map_digest(source.entities()) ||
        source.saved_revision_optional() != mixed_clipboard_placement_source_saved_revision(value))
        reject("captured snapshot/history source is stale or foreign");
    ReservedNames occupied;
    occupied.entities(source.entities());
    for (const auto& retained : source.history()) {
        occupied.entities(retained.entities);
        for (const auto& [id, asset] : retained.assets) { occupied.text(id); occupied.json(asset.metadata); }
        // Captured passive clone content and declared destinations remain
        // reserved even when they never became materialized rows, or Undo
        // returned to a state before their original command.
        if (retained.boundary_constraint_changes)
            occupied.json(command_to_json(Command{*retained.boundary_constraint_changes}));
        if (retained.boundary_geometry_edit)
            occupied.json(encode_boundary_geometry_edit(*retained.boundary_geometry_edit));
    }
    for (const auto& [id, asset] : source.assets()) { occupied.text(id); occupied.json(asset.metadata); }
    if (!value.at("roof_authoring").is_null()) occupied.json(value.at("roof_authoring"));
    auto stage = roof_replay(source.entities(), value);
    const auto roof_names = roof_new_names(source.entities(), stage);
    for (const auto& name : roof_names) occupied.text(name);
    const auto additions = decode_additions(value.at("additions"), source.revision());
    for (const auto& change : additions.entity_changes) {
        if (!occupied.values.insert(change.entity.id).second)
            reject("fresh owner is reserved in source, history or roof stage");
        for (const auto& child : owned_children(change.entity))
            if (!occupied.values.insert(child).second)
                reject("fresh child is reserved in source, history or roof stage");
    }
    append_fresh(stage, source.entities(), additions);
    const auto aliases = embedded_assembly_presentation_ids(stage);
    const auto prior_aliases = embedded_assembly_presentation_ids(source.entities());
    for (const auto& [qualified, alias] : aliases) {
        if (prior_aliases.contains(qualified) || !std::any_of(additions.entity_changes.begin(),
                additions.entity_changes.end(), [&](const auto& change) { return change.entity.id == qualified.first; }))
            continue;
        if (!occupied.values.insert(alias).second) reject("fresh assembly alias is reserved in source/history");
    }
    for (const auto& [id, entity] : source.entities()) {
        (void)id;
        if (entity.type != kSheetViewEntityType || !entity.extensions.contains("pinc_import")) continue;
        const auto& metadata = entity.extensions.at("pinc_import");
        if (!metadata.is_object() || !metadata.contains("source_asset_id") ||
            !metadata.at("source_asset_id").is_string()) reject("invalid imported page source");
        const auto asset = source.assets().find(metadata.at("source_asset_id").get<std::string>());
        if (asset == source.assets().end() || asset->second.media_type != "application/x-pincsketch")
            reject("imported page source is missing");
        break; // Matches the actual first imported page carrier.
    }
    (void)pages(source.entities());
}
Json make_mixed_clipboard_placement_intent(const DocumentSnapshot& source, const Json& roof,
    const ApplyEntityChanges& additions, std::string_view selected_registry_id, std::string_view message) {
    additions_shape(additions);
    if (additions.expected_revision != source.revision()) reject("fresh additions bind a different source");
    // Budget the prospective wire layout before constructing any JSON copy,
    // wrapping a typed command or entering a roof codec.
    Budget budget;
    budget.object({"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "roof_authoring", "additions",
        "selected_registry_id", "message"}, 0);
    for (int i = 0; i < 3; ++i) { budget.node(1); budget.add(32); }
    for (int i = 0; i < 3; ++i) budget.string(std::string_view("0000000000000000000000000000000000000000000000000000000000000000"), 1);
    budget.json(roof, 1); budget.string(selected_registry_id, 1); budget.string(message, 1);
    budget.object({"version", "kind", "expected_revision", "message", "entity_changes", "asset_changes"}, 1);
    budget.node(2); budget.add(32); budget.node(2); budget.add(32);
    budget.string("apply_entity_changes", 2); budget.string(additions.message, 2);
    budget.array(additions.entity_changes.size(), 2);
    for (const auto& change : additions.entity_changes) {
        budget.object({"kind", "entity"}, 3); budget.string("upsert", 4); budget.entity(change.entity, 4);
    }
    budget.array(0, 2);
    Json value{{"version", 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"roof_authoring", roof}, {"additions", command_to_json(Command{additions})},
        {"selected_registry_id", std::string(selected_registry_id)}, {"message", std::string(message)}};
    validate_mixed_clipboard_placement_source(source, value);
    return value;
}

} // namespace sketch

