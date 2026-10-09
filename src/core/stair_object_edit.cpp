#include "sketch/stair_object_edit.hpp"

#include "sketch/building_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/vertical_levels.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Names = std::set<std::string, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t geometry_limit = 100000;

[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
const Json* field(const Json& value, const std::string& name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
}
void keys(const Json& value, const Names& names) {
    if (!value.is_object() || value.size() != names.size()) invalid("Stair edit fields are not closed");
    for (const auto& name : names) if (!value.contains(name)) invalid("Stair edit field is missing");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Stair edit identity must be a string");
    const auto& id = value.get_ref<const std::string&>();
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("Stair edit identity is invalid");
    return id;
}
double scalar(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) invalid("Stair edit scalar must be finite");
    return value.get<double>();
}
void nodes(const Json& value, std::size_t& count, std::size_t depth = 0) {
    if (++count > 65536 || depth > 32) invalid("Stair edit structural budget exceeded");
    if ((value.is_array() || value.is_object()) && value.size() > collection_limit)
        invalid("Stair edit collection budget exceeded");
    if (value.is_number_float()) (void)scalar(value);
    if (value.is_string() && value.get_ref<const std::string&>().size() > 16384)
        invalid("Stair edit string budget exceeded");
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        if (key.size() > 16384) invalid("Stair edit key budget exceeded");
        nodes(child, count, depth + 1);
    }
    else if (value.is_array()) for (const auto& child : value) nodes(child, count, depth + 1);
}
std::size_t budget(const Json& value) {
    std::size_t count = 0;
    nodes(value, count);
    const auto size = value.dump().size();
    if (size > proof_limit) invalid("Stair edit byte budget exceeded");
    return size;
}
Names root_fields(std::string_view type) {
    if (type == "stair") return {"version", "form", "base_position_m", "orientation_rad", "riser_count",
        "total_rise_m", "going_m", "width_m", "top_landing", "level_connection", "flights", "landings",
        "vertical_placement"};
    if (type == "railing") return {"version", "form", "base_position_m", "orientation_rad", "length_m",
        "height_m", "thickness_m", "post_spacing_m", "host", "vertical_placement"};
    invalid("Stair edit family is unsupported");
}
Names record_fields(std::string_view key, const Json& record, const Json& properties) {
    if (key == "flights") return {"id", "riser_count", "going_m", "width_m"};
    if (key == "landings") return {"id", "depth_m", "thickness_m", "turn", "return_gap_m", "straight_alignment"};
    if (key == "top_landing") return {"depth_m", "thickness_m"};
    if (key == "level_connection") return {"version", "graph_id", "link_id", "lower_level_id", "upper_level_id"};
    if (key == "vertical_placement") return {"version", "mode", "offset_m"};
    if (key == "host") {
        if (properties.at("version") == 2) return {"stair_id", "flight_id", "side", "start_fraction", "end_fraction"};
        if (properties.at("version") == 3) {
            if (record.at("role") == "top") return {"stair_id", "role", "incoming_flight_id", "edge_index",
                "start_fraction", "end_fraction"};
            return {"stair_id", "role", "landing_id", "incoming_flight_id", "outgoing_flight_id", "edge_index",
                "start_fraction", "end_fraction"};
        }
    }
    invalid("Stair edit nested form is unsupported");
}
bool optional_record_field(std::string_view name) {
    return name == "going_m" || name == "width_m" || name == "straight_alignment";
}
// Complete typed projection. Missing optional children are represented explicitly
// in the wire while opaque source keys never enter the authoring descriptor.
Json project_record(std::string_view key, const Json& raw, const Json& properties) {
    Json result = Json::object();
    for (const auto& name : record_fields(key, raw, properties))
        result[name] = raw.contains(name) ? raw.at(name) : Json(nullptr);
    return result;
}
Json project_profile(const Entity& entity) {
    Json result = Json::object();
    for (const auto& name : root_fields(entity.type)) {
        const auto raw = field(entity.properties, name);
        if (!raw) { result[name] = nullptr; continue; }
        if (name == "flights" || name == "landings") {
            result[name] = Json::array();
            for (const auto& child : *raw) result[name].push_back(project_record(name, child, entity.properties));
        } else if (raw->is_object()) result[name] = project_record(name, *raw, entity.properties);
        else result[name] = *raw;
    }
    return result;
}
void placement(const Json& value) {
    keys(value, {"version", "mode", "offset_m"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
        (value.at("mode") != "absolute" && value.at("mode") != "level") ||
        std::abs(scalar(value.at("offset_m"))) > 1e9) invalid("Stair edit placement is unsupported");
}
// Validates wire shape without any synthetic entity map or native geometry.
std::string profile_type(const Json& profile, const std::string& id) {
    const auto form = field(profile, "form");
    if (!form || !form->is_string()) invalid("Stair edit profile requires its form");
    const std::string type = (*form == "straight_stair_flight" || *form == "multi_flight_stair") ? "stair" : "railing";
    keys(profile, root_fields(type));
    auto physical = profile;
    for (const auto& [name, raw] : profile.items()) {
        if (raw.is_null()) {
            if (name != "top_landing") physical.erase(name);
            continue;
        }
        if (name == "flights" || name == "landings") {
            if (!raw.is_array()) invalid("Stair edit topology must be an array");
            for (std::size_t i = 0; i < raw.size(); ++i) {
                keys(raw.at(i), record_fields(name, raw.at(i), profile));
                for (const auto& [key, value] : raw.at(i).items()) if (value.is_null()) {
                    if (!optional_record_field(key)) invalid("Stair edit required child field is null");
                    physical[name].at(i).erase(key);
                }
            }
        } else if (raw.is_object()) {
            keys(raw, record_fields(name, raw, profile));
            for (const auto& [key, value] : raw.items()) if (value.is_null())
                invalid("Stair edit required nested field is null");
        }
    }
    if (const auto raw = field(physical, "vertical_placement")) placement(*raw);
    if (type == std::string_view("stair")) (void)decode_stair_properties(id, physical);
    else {
        const auto rail = decode_railing_properties(id, physical);
        if ((rail.host || rail.landing_host) && physical.contains("vertical_placement"))
            invalid("Hosted stair rail cannot own placement");
    }
    return type;
}
void source_entity(const Entity& entity) {
    (void)identity(entity.id);
    if (!entity.properties.is_object() || !entity.extensions.is_object()) invalid("Stair edit source envelope is invalid");
    // Codecs refuse affected opaque, future, cross-family and malformed forms.
    if (entity.type == "stair") (void)decode_stair_properties(entity.id, entity.properties);
    else if (entity.type == "railing") (void)decode_railing_properties(entity.id, entity.properties);
    else invalid("Stair edit requires an actual stair or railing");
    if (const auto raw = field(entity.properties, "vertical_placement")) placement(*raw);
}
Json numeric_preserving(const Json* old, const Json& current, bool preserve) {
    if (preserve && old && old->is_number() && current.is_number() && *old == current) return *old;
    if (old && old->is_array() && current.is_array() && old->size() == current.size()) {
        auto result = current;
        for (std::size_t i = 0; i < current.size(); ++i) result.at(i) = numeric_preserving(&old->at(i), current.at(i), preserve);
        return result;
    }
    return current;
}
Json merge_record(std::string_view key, const Json* old, const Json& requested,
                  const Json& before, const Json& after, bool preserve) {
    keys(requested, record_fields(key, requested, after));
    const auto old_owned = old ? record_fields(key, *old, before) : Names{};
    auto result = old ? *old : Json::object();
    for (const auto& name : old_owned) result.erase(name);
    for (const auto& [name, raw] : requested.items()) {
        const auto previous = old ? field(*old, name) : nullptr;
        if (previous && !old_owned.contains(name)) invalid("Stair edit would reinterpret opaque nested authority");
        if (!raw.is_null()) result[name] = numeric_preserving(previous, raw, preserve);
    }
    return result;
}
const Json* entries(const Entity& entity) {
    const auto raw = field(entity.properties, "quantity_entries");
    if (raw && (!raw->is_object() || raw->size() > collection_limit)) invalid("Stair quantities must be a bounded object");
    if (raw) (void)budget(*raw);
    return raw;
}
// Every understood entered quantity is a length. Counts, angles, host stations
// and arbitrary JSON pointers never become measurement authority here.
Names scalar_pointers(const Entity& entity) {
    Names result;
    const Names dimensions = entity.type == "stair" ? Names{"total_rise_m", "going_m", "width_m"}
        : Names{"length_m", "height_m", "thickness_m", "post_spacing_m"};
    for (const auto& name : dimensions)
        if (entity.properties.contains(name)) result.insert(std::string("/") + name);
    if (entity.properties.contains("base_position_m")) for (int i = 0; i < 3; ++i)
        result.insert("/base_position_m/" + std::to_string(i));
    for (const auto* key : {"top_landing", "vertical_placement"}) if (const auto raw = field(entity.properties, key); raw && raw->is_object()) {
        if (std::string_view(key) == "top_landing" && entity.type != "stair") continue;
        const Names nested_dimensions = std::string_view(key) == "top_landing" ? Names{"depth_m", "thickness_m"} : Names{"offset_m"};
        for (const auto& name : nested_dimensions) if (raw->contains(name))
            result.insert(std::string("/") + key + "/" + name);
    }
    if (entity.type == "stair") for (const auto* key : {"flights", "landings"}) if (const auto raw = field(entity.properties, key); raw && raw->is_array()) {
        const Names child_dimensions = std::string_view(key) == "flights" ? Names{"going_m", "width_m"}
            : Names{"depth_m", "thickness_m", "return_gap_m"};
        for (std::size_t i = 0; i < raw->size(); ++i)
            for (const auto& name : child_dimensions)
                if (raw->at(i).contains(name)) result.insert(std::string("/") + key + "/" + std::to_string(i) + "/" + name);
    }
    return result;
}
// Resolve receipt ownership from the actual old typed row, never its new index.
std::optional<std::string> relocated_pointer(const Entity& before, const Entity& after, const std::string& pointer) {
    for (const auto* key : {"flights", "landings"}) {
        const auto prefix = std::string("/") + key + "/";
        if (!pointer.starts_with(prefix)) continue;
        const auto old = field(before.properties, key), current = field(after.properties, key);
        if (!old || !old->is_array()) return std::nullopt;
        for (std::size_t i = 0; i < old->size(); ++i) {
            const auto row_prefix = prefix + std::to_string(i) + "/";
            if (!pointer.starts_with(row_prefix)) continue;
            if (!current || !current->is_array()) return std::nullopt;
            for (std::size_t j = 0; j < current->size(); ++j)
                if (old->at(i).at("id") == current->at(j).at("id"))
                    return prefix + std::to_string(j) + pointer.substr((prefix + std::to_string(i)).size());
            return std::nullopt;
        }
        return std::nullopt;
    }
    return pointer;
}
const Json* at_scalar(const Entity& entity, const std::string& pointer, const Names& understood) {
    if (!understood.contains(pointer)) return nullptr;
    const Json::json_pointer path(pointer);
    return entity.properties.contains(path) ? &entity.properties.at(path) : nullptr;
}
bool known_receipt(const Json& value) {
    const auto version = field(value, "version");
    return version && version->is_number_integer() && *version == 1;
}
bool opaque_binding_unchanged(const Entity& before, const Entity& after, const std::string& pointer) {
    for (const auto* key : {"flights", "landings", "host", "level_connection", "top_landing"}) {
        const auto root = std::string("/") + key;
        if (pointer != root && !pointer.starts_with(root + "/")) continue;
        const auto old = field(before.properties, key), current = field(after.properties, key);
        if (std::string_view(key) == "flights" || std::string_view(key) == "landings") {
            if (pointer == root) return (!old && !current) || (old && current && exact(*old, *current));
            const auto relocated = relocated_pointer(before, after, pointer);
            if (relocated) return *relocated == pointer;
            // An uninterpreted pointer without an actual old row is retained
            // only when its entire possible binding domain remains unchanged.
            return (!old && !current) || (old && current && exact(*old, *current));
        }
        if (std::string_view(key) == "top_landing")
            return old && current && old->is_object() == current->is_object();
        return (!old && !current) || (old && current && exact(*old, *current));
    }
    return true;
}
void entered_receipt(const Json* old, const Json& raw, const Json* actual) {
    if (!known_receipt(raw) || (old && !known_receipt(*old))) invalid("Stair edit cannot author or overwrite opaque receipts");
    if (!actual) invalid("Stair quantity has no owned actual dimension");
    const auto quantity = decode_constraint_quantity_receipt(raw);
    if (quantity.original_expression.size() > 4096 || quantity.metres != scalar(*actual)) invalid("Stair quantity receipt is stale");
    if (!old) {
        keys(raw, {"version", "original_expression", "entered_unit", "exact_metres"});
        keys(raw.at("exact_metres"), {"numerator", "denominator"});
    } else {
        auto permitted = *old;
        for (const auto* key : {"version", "original_expression", "entered_unit"}) permitted[key] = raw.at(key);
        for (const auto* key : {"numerator", "denominator"}) permitted["exact_metres"][key] = raw.at("exact_metres").at(key);
        if (!exact(permitted, raw)) invalid("Stair edit changed opaque receipt metadata");
    }
}
void apply_receipts(const Entity& source, Entity& result, const Json& requested) {
    const auto old = entries(source);
    const auto old_scalars = scalar_pointers(source), new_scalars = scalar_pointers(result);
    Json retained = Json::object();
    std::map<std::string, std::string, std::less<>> old_by_new;
    Names changed;
    if (old) for (const auto& [pointer, raw] : old->items()) {
        const auto relocated = relocated_pointer(source, result, pointer);
        if (!old_scalars.contains(pointer)) {
            if (!opaque_binding_unchanged(source, result, pointer)) invalid("Stair edit changes an opaque quantity binding");
            retained[pointer] = raw;
            if (!old_by_new.emplace(pointer, pointer).second) invalid("Stair edit quantity bindings collide");
            continue;
        }
        if (!relocated) continue; // removed actual child
        if (!old_by_new.emplace(*relocated, pointer).second) invalid("Stair edit quantity bindings collide");
        const auto before = at_scalar(source, pointer, old_scalars), after = at_scalar(result, *relocated, new_scalars);
        if (!after || scalar(*before) != scalar(*after)) changed.insert(*relocated);
        else retained[*relocated] = raw;
    }
    if (requested.is_null()) {
        if (!old) return;
        if (!old->empty() && retained.empty()) result.properties.erase("quantity_entries");
        else result.properties["quantity_entries"] = std::move(retained);
        return;
    }
    if (!requested.is_object()) invalid("Stair input quantities must be a complete map");
    for (const auto& [pointer, raw] : retained.items()) {
        const auto current = field(requested, pointer);
        if (!current) invalid("Stair edit removed an unaffected entered receipt");
        if (exact(raw, *current)) continue;
        if (!new_scalars.contains(pointer)) invalid("Stair edit changed an opaque quantity pointer");
    }
    for (const auto& [pointer, raw] : requested.items()) {
        const auto retained_raw = field(retained, pointer);
        if (retained_raw && exact(*retained_raw, raw)) continue;
        if (!new_scalars.contains(pointer)) invalid("Stair edit added an opaque quantity pointer");
        const auto previous = old_by_new.find(pointer);
        const auto old_raw = old && previous != old_by_new.end() ? field(*old, previous->second) : nullptr;
        if (old_raw && changed.contains(pointer) && exact(*old_raw, raw)) invalid("Stair edit retained a receipt for a changed dimension");
        entered_receipt(old_raw, raw, at_scalar(result, pointer, new_scalars));
    }
    result.properties["quantity_entries"] = requested;
}
Entity stage_profile(const Entity& source, const StairObjectEditIntent& intent, bool preserve_numeric = true) {
    source_entity(source);
    if (source.id != intent.object_id || profile_type(intent.profile_fields, intent.object_id) != source.type)
        invalid("Stair edit target differs from its actual owner or family");
    auto result = source;
    for (const auto& [name, raw] : intent.profile_fields.items()) {
        const auto old = field(source.properties, name);
        if (raw.is_null()) {
            if (name == "top_landing") result.properties[name] = nullptr;
            else result.properties.erase(name);
        } else if (name == "flights" || name == "landings") {
            Json rows = Json::array();
            for (const auto& row : raw) {
                const Json* previous = nullptr;
                if (old && old->is_array()) for (const auto& child : *old)
                    if (child.at("id") == row.at("id")) { previous = &child; break; }
                rows.push_back(merge_record(name, previous, row, source.properties, intent.profile_fields, preserve_numeric));
            }
            result.properties[name] = std::move(rows);
        } else if (raw.is_object()) {
            result.properties[name] = merge_record(name, old && old->is_object() ? old : nullptr, raw,
                source.properties, intent.profile_fields, preserve_numeric);
        } else result.properties[name] = numeric_preserving(old, raw, preserve_numeric);
    }
    apply_receipts(source, result, intent.quantity_entries);
    source_entity(result);
    return exact(source, result) ? source : result;
}

void connected_levels(const Entities& entities, const ProjectOrganization& organization, const StairFlight& stair) {
    if (!stair.level_connection) return;
    const auto& connection = *stair.level_connection;
    const auto graph = entities.find(connection.graph_entity_id);
    if (graph == entities.end() || graph->second.type != "vertical_levels") invalid("Stair edit requires its actual level graph");
    const auto model = VerticalLevelGraph::from_json(graph->second.properties.at("model"));
    const auto link = std::find_if(model.links().begin(), model.links().end(), [&](const auto& value) { return value.id == connection.link_id; });
    if (link == model.links().end() || link->lower_level_id != connection.lower_level_id ||
        link->upper_level_id != connection.upper_level_id || link->state == RelationshipState::disconnected)
        invalid("Stair edit has a missing, disconnected or mismatched level link");
    if (std::abs(stair.total_rise - model.floor_to_floor_height(connection.link_id)) > VerticalLevelGraph::height_tolerance_m)
        invalid("Stair edit rise differs from its actual connected levels");
    const auto& entity = entities.at(stair.id);
    const auto placement_raw = field(entity.properties, "vertical_placement");
    if (!placement_raw || placement_raw->at("mode") != "level") return;
    const auto context = organization.drawing_context(stair.id);
    if (!context || context->floor_id.empty()) invalid("Connected stair edit requires an actual floor");
    const auto binding = VerticalLevelBinding::from_json(entities.at(context->floor_id).properties.at("vertical_level_binding"));
    if (binding.graph_entity_id != connection.graph_entity_id || binding.level_id != connection.lower_level_id)
        invalid("Connected stair edit must use its actual lower level placement");
}
std::optional<std::string> host_id(const Entity& entity) {
    if (entity.type != "railing") return std::nullopt;
    const auto host = field(entity.properties, "host");
    const auto stair = host ? field(*host, "stair_id") : nullptr;
    if (!stair || !stair->is_string()) return std::nullopt;
    return stair->get<std::string>();
}
void admit(const Entities& entities, const Names& targets) {
    validate_stair_attachment_state(entities);
    auto affected = targets;
    for (const auto& id : targets) if (const auto host = host_id(entities.at(id))) affected.insert(*host);
    for (const auto& [id, entity] : entities) if (const auto host = host_id(entity); host && affected.contains(*host)) affected.insert(id);
    const auto organization = organize_project(entities);
    std::map<std::string, BuildingObject, std::less<>> physical;
    std::size_t work = 0;
    for (const auto& id : affected) {
        const auto found = entities.find(id);
        if (found == entities.end() || found->first != found->second.id) invalid("Stair edit affected owner is missing");
        const auto& entity = found->second;
        source_entity(entity);
        const auto node = organization.nodes.find(id);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty())) invalid("Stair edit drawing context is unresolved");
        const auto resolved = host_id(entity) ? entity : resolve_vertical_placement(entities, entity);
        // Decode pure semantics and budget all work before invoking native builders.
        std::size_t cost = 0;
        BuildingObject object;
        if (entity.type == "stair") {
            auto stair = decode_stair_properties(id, resolved.properties);
            connected_levels(entities, organization, stair);
            cost = stair.riser_count + stair.landings.size() + (stair.top_landing ? 1 : 0);
            object = std::move(stair);
        } else {
            auto rail = decode_railing_properties(id, resolved.properties);
            if (const auto host = host_id(entity)) {
                const auto& authored_host = entities.at(*host);
                source_entity(authored_host);
                const auto actual_host = resolve_vertical_placement(entities, authored_host);
                cost = derive_hosted_railing_layout(rail, decode_stair_properties(*host, actual_host.properties)).posts.size() + 1;
            } else cost = static_cast<std::size_t>(std::ceil(rail.length / rail.post_spacing)) + 2;
            object = std::move(rail);
        }
        if (cost > geometry_limit - work) invalid("Stair edit affected geometry budget exceeded");
        work += cost;
        physical.emplace(id, std::move(object));
    }
    for (const auto& [id, object] : physical) {
        (void)id;
        try {
            (void)make_building_shape(object, entities);
        } catch (const Standard_Failure& error) {
            const auto message = error.GetMessageString();
            throw std::invalid_argument(std::string("Stair edit native geometry is invalid: ") +
                (message ? message : "Open CASCADE failure"));
        } catch (const std::exception& error) {
            throw std::invalid_argument(std::string("Stair edit native geometry is invalid: ") + error.what());
        }
    }
}
} // namespace

Json encode_stair_object_edit_intent(const StairObjectEditIntent& intent) {
    (void)identity(intent.object_id);
    Json result{{"version", 1}, {"object_id", intent.object_id}, {"profile_fields", intent.profile_fields},
        {"quantity_entries", intent.quantity_entries}};
    (void)budget(result);
    if (!intent.quantity_entries.is_null() && !intent.quantity_entries.is_object()) invalid("Stair edit quantities must be an object or null");
    (void)profile_type(intent.profile_fields, intent.object_id);
    return result;
}
StairObjectEditIntent decode_stair_object_edit_intent(const Json& value) {
    (void)budget(value);
    keys(value, {"version", "object_id", "profile_fields", "quantity_entries"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1) invalid("Stair edit version is unsupported");
    StairObjectEditIntent result{identity(value.at("object_id")), value.at("profile_fields"), value.at("quantity_entries")};
    (void)encode_stair_object_edit_intent(result);
    return result;
}
Entities replay_stair_object_edit_entities(const Entities& source, const std::vector<StairObjectEditIntent>& intents) {
    if (intents.size() > collection_limit) invalid("Stair edit target budget exceeded");
    if (intents.empty()) return source;
    auto result = source;
    Names targets;
    const auto scope = constraint_phase_scope(source);
    std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto size = encode_stair_object_edit_intent(intent).dump().size();
        if (size > proof_limit - bytes) invalid("Stair edit batch byte budget exceeded");
        bytes += size;
        if (!targets.insert(intent.object_id).second) invalid("Stair edit contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.object_id)) invalid("Stair edit target is inactive in the saved design");
        const auto found = source.find(intent.object_id);
        if (found == source.end() || found->first != found->second.id) invalid("Stair edit actual target is missing");
        result.at(intent.object_id) = stage_profile(found->second, intent);
    }
    admit(source, targets);
    validate_stair_identity_transition(source, result, std::span<const RevisionRecord>{});
    admit(result, targets);
    return result;
}
std::optional<StairObjectEditIntent> capture_stair_object_edit(const Entity& original, const Entity& edited) {
    if (original.type != "stair" && original.type != "railing") return std::nullopt;
    source_entity(original);
    source_entity(edited);
    if (exact(original, edited)) return std::nullopt;
    StairObjectEditIntent result{original.id, project_profile(edited), nullptr};
    if (const auto raw = entries(edited)) result.quantity_entries = *raw;
    (void)encode_stair_object_edit_intent(result);
    // Rebuild using edited known encodings only for the capture comparison.
    // Metadata still comes exclusively from the actual original. Published
    // replay instead keeps raw unchanged source numeric representations.
    if (!exact(stage_profile(original, result, false), edited)) invalid("Stair capture changed fields outside typed profile authority");
    if (exact(stage_profile(original, result), original)) return std::nullopt;
    return result;
}
} // namespace sketch
