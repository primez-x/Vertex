#include "sketch/model_copy_composition.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_json_bytes = 1024 * 1024;
constexpr std::size_t maximum_map_bytes = 64 * 1024 * 1024;
constexpr std::size_t maximum_map_nodes = 4 * 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Independent model copy composition: " + reason);
}
bool exact_json(const Json& a, const Json& b) {
    return a == b && a.dump() == b.dump();
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact_json(a.properties, b.properties) && exact_json(a.extensions, b.extensions);
}
void identity(const std::string& id, std::size_t limit) {
    if (id.empty() || id.size() > limit || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("entity identity/type is outside its bounded ASCII vocabulary");
}
std::string reference(const Json& value) {
    if (!value.is_string()) reject("presentation identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > maximum_json_bytes ||
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }))
        reject("presentation identity must be bounded and nonblank");
    return text;
}

// Validate portability before any dump/comparison/codec work. Count envelopes,
// keys, strings and numeric/container values, including opaque future fields.
struct Budget {
    std::size_t nodes{}, strings{}, serialized{};
    void bytes(std::size_t count, std::size_t& used) {
        if (count > maximum_map_bytes - used) reject("complete map byte budget exceeded");
        used += count;
    }
    void read(const Json& value, std::size_t depth, std::size_t& local_nodes,
        std::size_t& local_strings) {
        if (depth > 64 || ++nodes > maximum_map_nodes || ++local_nodes > 100000)
            reject("JSON node/nesting budget exceeded");
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>())))
            reject("nonportable or nonfinite JSON value");
        const auto text = [&](const std::string& s) {
            if (s.size() > maximum_json_bytes - local_strings) reject("JSON string/key budget exceeded");
            local_strings += s.size(); bytes(s.size(), strings);
        };
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1, local_nodes, local_strings);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            if (key.size() > 128) reject("JSON object key exceeds 128 bytes");
            text(key); read(child, depth + 1, local_nodes, local_strings);
        }
    }
    void object(const Json& value) {
        if (!value.is_object()) reject("entity properties/extensions must be objects");
        std::size_t local_nodes{}, local_strings{};
        read(value, 0, local_nodes, local_strings);
        const auto size = value.dump().size();
        if (size > maximum_json_bytes) reject("JSON object serialized budget exceeded");
        bytes(size, serialized);
    }
};
void bounded_map(const ModelCopyEntities& entities) {
    if (entities.size() > maximum_entities) reject("complete map entity budget exceeded");
    Budget budget;
    for (const auto& [key, entity] : entities) {
        identity(key, 128);
        if (entity.type.empty() || entity.type.size() > 64 ||
            !std::all_of(entity.type.begin(), entity.type.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            })) reject("entity type is outside its bounded ASCII vocabulary");
        if (key != entity.id) reject("map key differs from entity identity");
        budget.bytes(key.size() + entity.id.size() + entity.type.size() + 34, budget.strings);
        // Includes map keys, all five envelope fields and punctuation/required.
        budget.bytes(Json(key).dump().size() + Json(entity.id).dump().size() +
            Json(entity.type).dump().size() + 80, budget.serialized);
        budget.nodes += 6;
        if (budget.nodes > maximum_map_nodes) reject("complete envelope node budget exceeded");
        budget.object(entity.properties); budget.object(entity.extensions);
    }
}

enum class Rows { membership, appearance, overlay, override_object };
struct AppendPath { Json::json_pointer path; Rows kind; };
std::vector<AppendPath> append_paths(const Entity& source) {
    std::vector<AppendPath> paths;
    if (source.type == kAnnotationEntityType) {
        validate_annotation_entity(source);
        paths.push_back({Json::json_pointer("/state/overrides"), Rows::override_object});
    } else if (source.type == kSheetViewEntityType) {
        validate_sheet_view_entity(source);
        const auto& views = source.properties.at("model").at("views");
        for (std::size_t i = 0; i < views.size(); ++i) {
            const auto prefix = "/model/views/" + std::to_string(i);
            const auto& view = views.at(i);
            if (view.contains("object_ids")) paths.push_back({Json::json_pointer(prefix + "/object_ids"), Rows::membership});
            const auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                paths.push_back({Json::json_pointer(prefix + "/presentation/appearance/objects"), Rows::appearance});
            if (view.contains("overlays")) paths.push_back({Json::json_pointer(prefix + "/overlays"), Rows::overlay});
        }
    } else reject("copy candidate changed an immutable source entity");
    return paths;
}
std::string destination(const Json& row, Rows kind) {
    switch (kind) {
    case Rows::membership: return reference(row);
    case Rows::appearance: return reference(row.at("object_id"));
    case Rows::overlay: return reference(row.at("id"));
    case Rows::override_object:
        // Distinct annotation kinds can share target spellings.
        return Json::array({row.at("target_kind"), row.at("target_id")}).dump();
    }
    reject("unknown append row kind");
}
void copied_row(const Json& row, const Json& retained, Rows kind, const Ids& fresh_owners) {
    if (kind == Rows::membership) {
        if (!fresh_owners.contains(reference(row))) reject("appended membership requires a fresh candidate owner");
        return;
    }
    if (kind == Rows::overlay) {
        bool copied_owner = false;
        if (row.contains("object_id") && row.at("object_id") != "")
            copied_owner = fresh_owners.contains(reference(row.at("object_id")));
        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
            copied_owner = copied_owner || fresh_owners.contains(reference(row.at("dimension_binding").at("object_id")));
        if (!copied_owner) reject("appended overlay requires a fresh candidate owner");
        const auto bound = [](const Json& value) {
            return value.contains("dimension_binding") && !value.at("dimension_binding").is_null();
        };
        const auto remainder = [&](Json value) {
            value.erase("id"); value.erase("object_id");
            if (bound(value)) value.at("dimension_binding").erase("object_id");
            else {
                // The independently admitted leaf owns projected geometry.
                // Binding offsets and every style/opaque sibling stay exact.
                value.erase("start_m"); value.erase("end_m");
            }
            return value;
        };
        const auto copy_remainder = remainder(row);
        if (std::none_of(retained.begin(), retained.end(), [&](const auto& original) {
            return bound(original) == bound(row) && exact_json(copy_remainder, remainder(original));
        })) reject("appended overlay changed its source style, binding or opaque fields");
        return;
    }
    const char* key = kind == Rows::appearance ? "object_id" : "target_id";
    if (kind == Rows::override_object && row.at("target_kind") != "object")
        reject("only object annotation overrides may append");
    if (!fresh_owners.contains(reference(row.at(key)))) reject("appended presentation requires a fresh candidate owner");
    auto remainder = row; remainder.erase(key);
    if (std::none_of(retained.begin(), retained.end(), [&](const auto& original) {
        auto source_row = original; source_row.erase(key);
        return exact_json(remainder, source_row);
    })) reject("appended appearance/annotation row is not a source-preserving copy");
}
void merge_presentation(Entity& merged, const Entity& source, const Entity& candidate,
    const Ids& fresh_owners) {
    const auto paths = append_paths(source);
    auto envelope = candidate;
    for (const auto& allowed : paths) {
        const auto& retained = source.properties.at(allowed.path);
        const auto& additions = candidate.properties.at(allowed.path);
        auto& final_rows = merged.properties.at(allowed.path);
        if (!retained.is_array() || !additions.is_array() || additions.size() < retained.size())
            reject("presentation copy must retain source array types and prefixes");
        for (std::size_t i = 0; i < retained.size(); ++i)
            if (!exact_json(retained.at(i), additions.at(i))) reject("retained presentation row changed or reordered");
        Ids destinations;
        for (const auto& row : final_rows)
            if (!destinations.insert(destination(row, allowed.kind)).second) reject("duplicate qualified presentation identity");
        for (std::size_t i = retained.size(); i < additions.size(); ++i) {
            const auto& row = additions.at(i);
            if (!destinations.insert(destination(row, allowed.kind)).second) reject("copy presentation destinations overlap");
            copied_row(row, retained, allowed.kind, fresh_owners);
            final_rows.push_back(row);
        }
        envelope.properties.at(allowed.path) = retained;
    }
    if (!exact(source, envelope)) reject("candidate changed fields outside known presentation append paths");
}
void retain_aliases(const EmbeddedAssemblyPresentationIds& expected,
    const EmbeddedAssemblyPresentationIds& final_aliases) {
    for (const auto& [key, alias] : expected) {
        const auto found = final_aliases.find(key);
        if (found == final_aliases.end() || found->second != alias)
            reject("composition would retarget a source or independent candidate presentation alias");
    }
}
Ids annotation_children(const ModelCopyEntities& entities) {
    Ids children;
    for (const auto& [key, entity] : entities) {
        (void)key;
        if (entity.type != kAnnotationEntityType) continue;
        validate_annotation_entity(entity);
        for (const auto* collection : {"labels", "symbols"})
            for (const auto& row : entity.properties.at("state").at(collection)) {
                const auto id = reference(row.at("id"));
                // Reservation preserves even an admitted source's repeated
                // spellings. It does not impose a new identity scheme on it.
                children.insert(id);
            }
    }
    return children;
}
void fresh_annotation_children(const ModelCopyEntities& actual, const ModelCopyEntities& result,
    const Ids& source_children, const EmbeddedAssemblyPresentationIds& aliases) {
    Ids fresh;
    Ids render_ids;
    for (const auto& [binding, alias] : aliases) { (void)binding; render_ids.insert(alias); }
    for (const auto& [key, entity] : result) {
        if (actual.contains(key) || entity.type != kAnnotationEntityType) continue;
        validate_annotation_entity(entity);
        for (const auto* collection : {"labels", "symbols"})
            for (const auto& row : entity.properties.at("state").at(collection)) {
                const auto id = reference(row.at("id"));
                if (result.contains(id) || source_children.contains(id) || render_ids.contains(id) ||
                    !fresh.insert(id).second)
                    reject("fresh annotation child collides with a document or annotation child identity");
            }
    }
}
void final_presentation_owners(const ModelCopyEntities& actual, const ModelCopyEntities& entities,
    const EmbeddedAssemblyPresentationIds& aliases) {
    Ids owners;
    for (const auto& [key, entity] : entities) { (void)entity; owners.insert(key); }
    for (const auto& [key, alias] : aliases) { (void)key; owners.insert(alias); }
    for (const auto& [key, entity] : entities) {
        const auto original = actual.find(key);
        if (entity.type == kSheetViewEntityType) {
            const auto model = decode_sheet_view_entity(entity);
            std::map<std::string, Ids, std::less<>> retained_overlays;
            if (original != actual.end())
                for (const auto& prior : original->second.properties.at("model").at("views")) {
                    auto& ids = retained_overlays[reference(prior.at("id"))];
                    if (prior.contains("overlays"))
                        for (const auto& row : prior.at("overlays")) ids.insert(reference(row.at("id")));
                }
            for (const auto& view : model.views()) {
                // Existing codec-admitted local view vocabulary is qualified by
                // this actual entity/view, never globally reserved by spelling.
                Ids local_owners;
                for (const auto& overlay : view.overlays) local_owners.insert(overlay.id);
                const auto require_owner = [&](const std::string& id) {
                    if (!owners.contains(id) && !local_owners.contains(id))
                        reject("final view references a missing qualified owner");
                };
                for (const auto& id : view.object_ids) require_owner(id);
                if (view.presentation.appearance) for (const auto& row : view.presentation.appearance->objects)
                    require_owner(row.object_id);
                // Optional associations may already be unresolved in admitted
                // source state. Prefix preservation owns those unchanged rows;
                // only new associations acquire copy-reference requirements.
                const auto prior = retained_overlays.find(view.id);
                for (const auto& row : view.overlays) {
                    const bool retained = prior != retained_overlays.end() && prior->second.contains(row.id);
                    if (!retained && !row.object_id.empty()) require_owner(row.object_id);
                    if (row.dimension_binding) require_owner(row.dimension_binding->object_id);
                }
            }
        } else if (entity.type == kAnnotationEntityType) {
            const auto state = decode_annotation_entity(entity);
            const auto retained = original == actual.end() ? 0 :
                original->second.properties.at("state").at("overrides").size();
            std::size_t index{};
            for (const auto& row : state.overrides)
                if (index++ >= retained && row.target_kind == "object" && !owners.contains(row.target_id))
                    reject("final object annotation override references a missing owner");
        } else if (entity.type == "assembly_model") {
            // Native catalog admission owns placement decoding/geometry. Check
            // the final actual host namespace as well, without reconstructing
            // or normalizing the raw catalog or its local definitions.
            for (const auto& row : entity.properties.at("model").at("instances")) {
                if (!row.contains("placement")) continue;
                const auto host = entities.find(reference(row.at("placement").at("host_entity_id")));
                if (host == entities.end()) reject("final hosted catalog instance references a missing entity");
                const auto& type = host->second.type;
                if (type != "boundary" && type != "measurement_boundary" && type != "room_boundary" &&
                    type != "wall" && type != "opening" && type != "slab" && type != "roof" &&
                    type != "stair" && type != "railing" && type != "column" && type != "beam" &&
                    type != "terrain_surface") reject("final catalog placement has an unsupported host type");
            }
        }
    }
}
} // namespace

ModelCopyEntities compose_independent_model_copy_candidates(const ModelCopyEntities& actual,
    const std::vector<ModelCopyEntities>& candidates) {
    try {
        if (candidates.empty() || candidates.size() > 4) reject("requires 1..4 independent candidates");
        bounded_map(actual);
        const auto source_aliases = embedded_assembly_presentation_ids(actual);
        const auto children = annotation_children(actual);
        auto result = actual;
        std::vector<EmbeddedAssemblyPresentationIds> leaf_aliases;
        leaf_aliases.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            bounded_map(candidate);
            const auto aliases = embedded_assembly_presentation_ids(candidate);
            retain_aliases(source_aliases, aliases);
            Ids fresh_owners;
            for (const auto& [key, entity] : candidate) if (!actual.contains(key)) {
                if (result.size() == maximum_entities) reject("final entity budget exceeded");
                if (children.contains(key) || !result.emplace(key, entity).second)
                    reject("fresh entity key collides with another candidate or annotation child");
                fresh_owners.insert(key);
            }
            for (const auto& [key, alias] : aliases)
                if (!source_aliases.contains(key)) fresh_owners.insert(alias);
            for (const auto& [key, original] : actual) {
                const auto found = candidate.find(key);
                if (found == candidate.end()) reject("candidate removed an actual source entity");
                if (!exact(original, found->second))
                    merge_presentation(result.at(key), original, found->second, fresh_owners);
            }
            leaf_aliases.push_back(aliases);
        }
        bounded_map(result);
        const auto aliases = embedded_assembly_presentation_ids(result);
        retain_aliases(source_aliases, aliases);
        for (const auto& leaf : leaf_aliases) retain_aliases(leaf, aliases);
        fresh_annotation_children(actual, result, children, aliases);
        final_presentation_owners(actual, result, aliases);
        validate_document_assembly_instances(result);
        return result;
    } catch (const Json::exception& error) {
        reject("malformed candidate JSON: " + std::string(error.what()));
    }
}

} // namespace sketch
