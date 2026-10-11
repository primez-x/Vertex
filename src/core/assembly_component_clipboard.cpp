#include "sketch/assembly_component_clipboard.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/corner_window.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_requests = 128;
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void document_id(std::string_view value) {
    require(!value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "component clipboard requires an actual document identity");
}
void local_id(std::string_view value) {
    require(!value.empty() && value.size() <= 4096 && value.find('\0') == std::string_view::npos &&
        !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }),
        "component clipboard requires a bounded nonblank local identity without NUL");
}
const std::string& string(const Json& value) {
    require(value.is_string(), "component clipboard requires a string");
    return value.get_ref<const std::string&>();
}
void fields(const Json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "component clipboard has unsupported fields");
    for (const auto* key : keys) require(value.contains(key), "component clipboard field is missing");
}
void digest_shape(const Json& value) {
    const auto& bytes = string(value);
    require(bytes.size() == 64 && std::all_of(bytes.begin(), bytes.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "component clipboard digest must be lowercase SHA256");
}
// Allocation-free JSON framing. Source and passive records have separate caps;
// every source carrier is admitted before a model, phase or presentation decode.
struct Budget {
    std::size_t bytes{}, values{};
    std::size_t byte_limit{4 * 1024 * 1024}, value_limit{100000};
    void add(std::size_t count) {
        require(count <= byte_limit - bytes, "component clipboard JSON byte budget exceeded");
        bytes += count;
    }
    void node(std::size_t depth) {
        require(depth <= 64 && ++values <= value_limit, "component clipboard JSON value/depth budget exceeded");
    }
    void quoted(std::string_view value) {
        add(2); add(value.size());
        for (unsigned char c : value) {
            if (c < 0x20) add(5);
            else if (c == '"' || c == '\\') add(1);
        }
    }
    void json(const Json& value, std::size_t depth = 0) {
        node(depth);
        require(!value.is_binary() && !value.is_discarded() &&
            (!value.is_number_float() || std::isfinite(value.get<double>())),
            "component clipboard requires finite ordinary JSON");
        if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else if (value.is_object()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); json(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& child : value) json(child, depth + 1);
        } else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
    void entity(const Entity& entity) {
        // Conservative complete five-field envelope, including names/commas.
        add(128); node(0); quoted(entity.id); quoted(entity.type); node(1); node(1); node(1);
        json(entity.properties, 1); json(entity.extensions, 1);
    }
    void source(const AssemblyDocumentEntities& entities) {
        require(entities.size() <= 100000, "component clipboard source entity budget exceeded");
        for (const auto& [id, entity] : entities) {
            document_id(id);
            require(entity.id == id, "component clipboard source identity differs from its map key");
            this->entity(entity);
        }
    }
    void record(const AssemblyClipboardDetachRequest& request, const Json& row) {
        // Seven-field envelope and the largest six-field phase envelope. Reserve
        // all values/escaping before copying the raw source row into a record.
        // Five ASCII host IDs, carrier IDs, digest strings and all fixed keys
        // fit this reserve. The actual saved alternative is charged separately
        // for every declared row before any record or digest is allocated.
        add(2048); node(0);
        for (int i = 0; i < 20; ++i) node(1);
        quoted(request.catalog_id); quoted(request.instance_id); quoted(request.independent_id);
        json(row, 2);
    }
};
const Json& model(const Entity& entity, std::string_view type) {
    require(entity.type == type && entity.properties.is_object() &&
        entity.properties.contains("model") && entity.properties.at("model").is_object(),
        "component clipboard needs an actual typed model carrier");
    return entity.properties.at("model");
}
std::string digest(const Entity& entity) {
    // nlohmann::json uses sorted object keys; include the complete carrier,
    // required flag and opaque metadata, not just its semantic model.
    const auto bytes = Json{{"id", entity.id}, {"type", entity.type}, {"required", entity.required},
        {"properties", entity.properties}, {"extensions", entity.extensions}}.dump();
    return sha256_hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
}
AssemblyInstance normalized(AssemblyInstance instance, const std::string& wire_id) {
    AssemblyTransform transform;
    if (instance.root_transform) transform = *instance.root_transform;
    else if (instance.placement) {
        const auto& pose = *instance.placement;
        transform = {{pose.translation_m.x, pose.translation_m.y, pose.translation_z_m},
            pose.rotation_radians, pose.scale, pose.mirrored_y, pose.vertical_scale};
    }
    instance.id = wire_id;
    instance.root_transform = transform;
    instance.placement.reset();
    return instance;
}
struct Phases {
    std::map<std::string, ModelPhases, std::less<>> registries;
    std::map<std::string, std::string, std::less<>> owners;
    std::set<std::string, std::less<>> inactive;
    explicit Phases(const AssemblyDocumentEntities& entities) {
        for (const auto& [id, entity] : entities) {
            if (entity.type != "model_phases") continue;
            auto phase = ModelPhases::from_json(model(entity, "model_phases"));
            const auto states = phase.active_state();
            for (const auto& owner : phase.entity_ids()) {
                const auto found = entities.find(owner);
                require(found != entities.end() && is_model_phase_entity_type(found->second.type),
                    "component clipboard phase registry has a missing or mistyped owner");
                require(owners.emplace(owner, id).second, "component clipboard owner belongs to overlapping registries");
                const auto state = states.find(owner);
                if (state == states.end() || state->second == ModelPhase::demolished) inactive.insert(owner);
            }
            registries.emplace(id, std::move(phase));
        }
    }
    void active(const std::string& owner, std::optional<std::string>& registry) const {
        require(!inactive.contains(owner), "component clipboard reached an inactive catalog or host");
        const auto found = owners.find(owner);
        if (found == owners.end()) return;
        require(!registry || *registry == found->second, "component clipboard reached a foreign physical registry");
        registry = found->second;
    }
};
std::vector<std::string> host_closure(const AssemblyDocumentEntities& entities, const Phases& phases,
    const AssemblyInstance& instance, std::optional<std::string>& registry) {
    std::set<std::string, std::less<>> hosts;
    const auto actual = [&](const std::string& id, std::string_view type) -> const Entity& {
        document_id(id);
        const auto found = entities.find(id);
        require(found != entities.end() && (type.empty() || found->second.type == type),
            "component clipboard inherited host is missing or mistyped");
        phases.active(id, registry); hosts.insert(id);
        return found->second;
    };
    const auto opening_wall = [&](const Entity& opening) {
        require(opening.type == "opening" && opening.properties.is_object() &&
            opening.properties.contains("wall_id"), "component clipboard opening has no actual wall");
        (void)actual(string(opening.properties.at("wall_id")), "wall");
    };
    const auto corner = [&](const std::string& id) {
        const auto& owner = actual(id, "corner_window");
        const auto value = parse_corner_window(owner);
        for (std::size_t leg = 0; leg < value.opening_ids.size(); ++leg) {
            const auto& cut = actual(value.opening_ids[leg], "opening");
            require(cut.properties.is_object() && cut.properties.contains("corner_window_id") &&
                cut.properties.at("corner_window_id") == id && cut.properties.contains("corner_leg") &&
                cut.properties.at("corner_leg").is_number_integer() && cut.properties.at("corner_leg") == leg &&
                cut.properties.contains("wall_id") && cut.properties.at("wall_id") == value.wall_ids[leg],
                "component clipboard corner has inconsistent actual cuts");
            opening_wall(cut);
        }
        return value;
    };
    if (instance.placement) {
        const auto& host = actual(instance.placement->host_entity_id, {});
        require(is_complete_assembly_catalog_host_type(host.type) || host.type == "corner_window",
            "component clipboard placement needs an actual supported host");
        if (host.type == "corner_window") (void)corner(host.id);
        else if (host.type == "opening") {
            opening_wall(host);
            if (host.properties.contains("corner_window_id")) {
                const auto value = corner(string(host.properties.at("corner_window_id")));
                require(std::find(value.opening_ids.begin(), value.opening_ids.end(), host.id) != value.opening_ids.end(),
                    "component clipboard cut is absent from its actual corner owner");
            }
        }
    }
    return {hosts.begin(), hosts.end()};
}
void admit_catalogs(const AssemblyDocumentEntities& entities) {
    AssemblyCatalogTransferBudget catalog_budget;
    for (const auto& [id, entity] : entities) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalog_budget);
    }
}
} // namespace

void validate_assembly_component_clipboard_provenance(const Json& records) {
    try {
        Budget budget; budget.json(records);
        require(records.is_array() && records.size() <= max_requests, "component clipboard provenance roster exceeds its limit");
        std::set<std::string, std::less<>> wire_ids;
        std::set<AssemblyPhaseRoot> source_ids;
        std::map<std::string, std::string, std::less<>> catalog_digests;
        std::map<std::string, Json, std::less<>> memberships;
        std::map<std::string, std::pair<Json, Json>, std::less<>> registries;
        std::map<std::string, std::string_view, std::less<>> source_roles;
        const auto source_role = [&](const std::string& id, std::string_view role) {
            const auto [found, added] = source_roles.emplace(id, role);
            require(added || found->second == role, "component clipboard source carrier roles collide");
        };
        for (const auto& row : records) {
            fields(row, {"wire_owner_id", "source_catalog_id", "source_instance_id", "source_catalog_digest",
                "source_row", "source_hosts", "source_phase"});
            const auto& wire = string(row.at("wire_owner_id")); document_id(wire);
            const auto& catalog = string(row.at("source_catalog_id")); document_id(catalog);
            source_role(catalog, "catalog");
            const auto& instance = string(row.at("source_instance_id")); local_id(instance);
            require(wire != catalog && wire_ids.insert(wire).second && source_ids.emplace(catalog, instance).second,
                "component clipboard provenance has colliding owners or qualified sources");
            digest_shape(row.at("source_catalog_digest"));
            const auto [catalog_entry, catalog_added] = catalog_digests.emplace(catalog, string(row.at("source_catalog_digest")));
            require(catalog_added || catalog_entry->second == row.at("source_catalog_digest"),
                "component clipboard catalog digest disagrees across rows");
            const auto& raw = row.at("source_row");
            require(raw.is_object() && raw.contains("id") && raw.at("id") == instance,
                "component clipboard raw source row identity disagrees");
            const auto& hosts = row.at("source_hosts");
            require(hosts.is_array() && hosts.size() <= 5, "component clipboard source host closure exceeds its limit");
            const std::string* previous = nullptr;
            for (const auto& host : hosts) {
                const auto& id = string(host); document_id(id);
                source_role(id, "host");
                require(!previous || *previous < id, "component clipboard host closure must be sorted and unique");
                previous = &id;
            }
            if (raw.contains("placement") && !raw.at("placement").is_null()) {
                const auto& placement = raw.at("placement");
                require(placement.is_object() && placement.contains("host_entity_id"), "component clipboard source placement is malformed");
                const auto& host = string(placement.at("host_entity_id")); document_id(host);
                require(std::any_of(hosts.begin(), hosts.end(), [&](const auto& value) { return value == host; }),
                    "component clipboard source host closure omits its placement host");
            } else require(hosts.empty(), "component clipboard unhosted source has an invented host closure");
            const auto& phase = row.at("source_phase");
            if (phase.is_null()) continue;
            fields(phase, {"membership_id", "registry_id", "active_alternative", "state", "membership_digest", "registry_digest"});
            const auto& membership = string(phase.at("membership_id")); document_id(membership);
            const auto& registry = string(phase.at("registry_id")); document_id(registry);
            source_role(membership, "membership"); source_role(registry, "registry");
            require(membership != registry && membership != catalog && registry != catalog &&
                membership != wire && registry != wire, "component clipboard phase carrier identities collide");
            if (!phase.at("active_alternative").is_null()) local_id(string(phase.at("active_alternative")));
            const auto& state = string(phase.at("state"));
            require(state == "existing" || (state == "proposed" && !phase.at("active_alternative").is_null()),
                "component clipboard phase provenance must describe an active source row");
            digest_shape(phase.at("membership_digest")); digest_shape(phase.at("registry_digest"));
            // Rows in one companion may have different states, but its source
            // identity, selected alternative and envelope digests must agree.
            auto binding = phase; binding.erase("state");
            const auto [member_entry, member_added] = memberships.emplace(membership, std::move(binding));
            auto expected = phase; expected.erase("state");
            require(member_added || member_entry->second == expected, "component clipboard companion provenance disagrees");
            const auto [registry_entry, registry_added] = registries.emplace(registry,
                std::pair{phase.at("active_alternative"), phase.at("registry_digest")});
            require(registry_added || registry_entry->second == std::pair{phase.at("active_alternative"), phase.at("registry_digest")},
                "component clipboard registry provenance disagrees");
        }
        for (const auto& id : wire_ids)
            require(!source_roles.contains(id), "component clipboard fresh wire owner aliases an actual source carrier");
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid component clipboard provenance: ") + error.what());
    }
}

void validate_assembly_component_clipboard_source_rows(const Json& records, const std::vector<Entity>& wire_entities) {
    validate_assembly_component_clipboard_provenance(records);
    try {
        require(wire_entities.size() <= 100000, "component clipboard wire entity budget exceeded");
        Budget budget; budget.json(records);
        for (const auto& entity : wire_entities) budget.entity(entity);
        AssemblyDocumentEntities wire;
        for (const auto& entity : wire_entities) {
            document_id(entity.id);
            require(wire.emplace(entity.id, entity).second, "component clipboard wire owners overlap");
        }
        AssemblyCatalogTransferBudget source_work;
        for (const auto& [id, entity] : wire) {
            (void)id;
            if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, source_work);
        }
        AssemblyExpansionBudget expansion_budget;
        for (const auto& row : records) {
            const auto& owner = string(row.at("wire_owner_id"));
            const auto found = wire.find(owner);
            require(found != wire.end() && !found->second.required && found->second.type == "assembly_instance",
                "component clipboard provenance has no independent nonrequired wire owner");
            const auto actual = decode_document_assembly_instance(found->second);
            const auto& catalog_id = string(row.at("source_catalog_id"));
            require(actual.assembly_catalog_id == catalog_id, "component clipboard source catalog reference disagrees");
            const auto catalog = wire.find(catalog_id);
            require(catalog != wire.end(), "component clipboard minimal wire catalog is missing");
            const auto& raw_model = model(catalog->second, "assembly_model");
            require(raw_model.contains("instances") && raw_model.at("instances").is_array() && raw_model.at("instances").empty(),
                "component clipboard source row needs a minimal catalog without embedded instances");
            // The actual catalog dialect decodes the saved local row. The wire
            // root is compared as a semantic value before destination movement.
            auto source_model = raw_model;
            source_model.at("instances").push_back(row.at("source_row"));
            Entity source_catalog = catalog->second;
            source_catalog.properties.at("model") = source_model;
            admit_existing_assembly_catalog_work(source_catalog, source_work);
            const auto decoded = AssemblyModel::from_json(source_model);
            require(decoded.instances().size() == 1 && normalized(decoded.instances().front(), owner) == actual.instance,
                "component clipboard wire root differs from its normalized raw source row");
            const auto expanded = decoded.expand(actual.instance, expansion_budget);
            require(!expanded.nodes.empty() && !expanded.profiles.empty(), "component clipboard root has no genuine geometry");
        }
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid component clipboard source rows: ") + error.what());
    }
}

void validate_assembly_component_clipboard_destinations(const Json& mapped_records,
    const AssemblyDocumentEntities& additions, const AssemblyDocumentEntities& fallback) {
    try {
        Budget budget; budget.json(mapped_records);
        require(mapped_records.is_array() && mapped_records.size()<=max_requests,
            "component clipboard destination roster exceeds its limit");
        Json sources=Json::array();
        std::set<std::string,std::less<>> destinations, admitted_catalogs;
        AssemblyCatalogTransferBudget catalog_work;
        AssemblyExpansionBudget expansion_work;
        for (const auto& mapping:mapped_records) {
            fields(mapping,{"source","destination_owner_id"});
            const auto& id=string(mapping.at("destination_owner_id")); document_id(id);
            require(destinations.insert(id).second,"component clipboard destination identities overlap");
            const auto owner=additions.find(id);
            require(owner!=additions.end() && owner->second.type=="assembly_instance" && !owner->second.required,
                "component clipboard destination must be a fresh independent addition");
            budget.entity(owner->second);
            const auto actual=decode_document_assembly_instance(owner->second);
            require(actual.instance.id==id && !actual.instance.placement && actual.instance.root_transform.has_value(),
                "component clipboard destination must retain an independent root pose");
            const auto local=additions.find(actual.assembly_catalog_id);
            const auto retained=fallback.find(actual.assembly_catalog_id);
            require(local!=additions.end() || retained!=fallback.end(),
                "component clipboard destination catalog is missing");
            const auto& catalog=local!=additions.end() ? local->second : retained->second;
            require(catalog.id==actual.assembly_catalog_id,
                "component clipboard destination catalog identity differs from its binding");
            if (admitted_catalogs.insert(catalog.id).second) {
                budget.entity(catalog);
                admit_existing_assembly_catalog_work(catalog,catalog_work);
            }
            const auto& source=mapping.at("source");
            auto raw_model=model(catalog,"assembly_model");
            raw_model.at("instances")=Json::array({source.at("source_row")});
            auto carrier=catalog; carrier.properties.at("model")=raw_model;
            admit_existing_assembly_catalog_work(carrier,catalog_work);
            const auto decoded=AssemblyModel::from_json(raw_model);
            require(decoded.instances().size()==1,"component clipboard destination source row is invalid");
            auto expected=normalized(decoded.instances().front(),id);
            expected.root_transform->translation_m.x=actual.instance.root_transform->translation_m.x;
            expected.root_transform->translation_m.y=actual.instance.root_transform->translation_m.y;
            require(expected==actual.instance,
                "component clipboard destination changed source type, overrides or non-XY pose");
            const auto expanded=decoded.expand(actual.instance,expansion_work);
            require(!expanded.nodes.empty() && !expanded.profiles.empty(),
                "component clipboard destination has no genuine source geometry");
            sources.push_back(source);
        }
        validate_assembly_component_clipboard_provenance(sources);
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid component clipboard destinations: ")+error.what());
    }
}

AssemblyClipboardDetachedCapture capture_assembly_component_clipboard(const DocumentSnapshot& source,
    const std::vector<AssemblyClipboardDetachRequest>& requests, const std::vector<Entity>& independent_roots) {
    try {
        require(requests.size() <= max_requests && independent_roots.size() <= max_requests &&
            requests.size() + independent_roots.size() <= max_requests, "component clipboard root roster exceeds its limit");
        const auto& entities = source.entities();
        Budget source_budget{0, 0, 64 * 1024 * 1024, 4 * 1024 * 1024};
        source_budget.source(entities);
        admit_catalogs(entities);
        // Reserve the complete prospective passive record before copying any
        // row or serializing a digest. Lookups here inspect only raw row IDs.
        Budget record_budget; record_budget.node(0); record_budget.add(2 + requests.size());
        std::set<AssemblyPhaseRoot> selected;
        std::set<std::string, std::less<>> fresh;
        std::vector<const Json*> raw_rows;
        for (const auto& request : requests) {
            document_id(request.catalog_id); local_id(request.instance_id); document_id(request.independent_id);
            require(selected.emplace(request.catalog_id, request.instance_id).second &&
                fresh.insert(request.independent_id).second && !entities.contains(request.independent_id),
                "component clipboard source is duplicated or its independent identity is not fresh");
            const auto found = entities.find(request.catalog_id);
            require(found != entities.end(), "component clipboard source catalog is missing");
            const auto& raw = model(found->second, "assembly_model");
            require(raw.contains("instances") && raw.at("instances").is_array(), "component clipboard source rows are missing");
            const auto row = std::find_if(raw.at("instances").begin(), raw.at("instances").end(), [&](const auto& value) {
                return value.is_object() && value.contains("id") && value.at("id") == request.instance_id;
            });
            require(row != raw.at("instances").end(), "component clipboard actual qualified source row is missing");
            record_budget.record(request, *row); raw_rows.push_back(&*row);
        }
        for (const auto& entity : independent_roots) {
            const auto found = entities.find(entity.id);
            require(found != entities.end() && found->second == entity &&
                found->second.properties.dump() == entity.properties.dump() &&
                found->second.extensions.dump() == entity.extensions.dump(),
                "component clipboard supplied independent root is not the exact actual source entity");
            require(fresh.insert(entity.id).second && !entity.required, "component clipboard independent roots collide or are required");
            (void)decode_document_assembly_instance(entity);
        }
        const auto presentations = embedded_assembly_presentation_ids(entities);
        for (const auto& [root, id] : presentations) {
            (void)root; require(!fresh.contains(id), "component clipboard independent identity collides with a presentation");
        }
        for (const auto& [id, entity] : entities) {
            (void)id;
            if (entity.type != kAnnotationEntityType) continue;
            const auto annotation = decode_annotation_entity(entity);
            for (const auto& child : annotation.labels) require(!fresh.contains(child.id), "component clipboard identity collides with an annotation");
            for (const auto& child : annotation.symbols) require(!fresh.contains(child.id), "component clipboard identity collides with a component");
        }
        validate_document_assembly_instances(entities);
        const auto membership = resolve_assembly_phase_memberships(entities);
        const Phases phases(entities);
        for (const auto& request : requests) {
            const auto declared = membership.owners.find({request.catalog_id, request.instance_id});
            if (declared == membership.owners.end()) continue;
            const auto& alternative = phases.registries.at(declared->second.registry_id).active_alternative();
            if (alternative) { local_id(*alternative); record_budget.quoted(*alternative); }
        }
        for (const auto& entity : independent_roots) {
            const auto value = decode_document_assembly_instance(entity);
            std::optional<std::string> registry;
            phases.active(entity.id, registry);
            phases.active(value.assembly_catalog_id, registry);
        }
        AssemblyClipboardDetachedCapture result;
        std::map<std::string, AssemblyModel, std::less<>> decoded_catalogs;
        std::map<std::string, std::string, std::less<>> digests;
        const auto source_digest = [&](const std::string& id) -> const std::string& {
            const auto found = digests.find(id);
            if (found != digests.end()) return found->second;
            return digests.emplace(id, digest(entities.at(id))).first->second;
        };
        AssemblyExpansionBudget selected_budget;
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto& request = requests[index];
            const auto& catalog = entities.at(request.catalog_id);
            if (!decoded_catalogs.contains(request.catalog_id))
                decoded_catalogs.emplace(request.catalog_id, AssemblyModel::from_json(model(catalog, "assembly_model")));
            const auto& decoded = decoded_catalogs.at(request.catalog_id);
            const auto row = std::find_if(decoded.instances().begin(), decoded.instances().end(), [&](const auto& value) {
                return value.id == request.instance_id;
            });
            require(row != decoded.instances().end(), "component clipboard actual row did not decode");
            const AssemblyPhaseRoot root{request.catalog_id, request.instance_id};
            const auto declared = membership.owners.find(root);
            std::optional<std::string> registry;
            Json phase = nullptr;
            if (declared != membership.owners.end()) {
                const auto state = membership.states.find(root);
                require(state != membership.states.end() && state->second != ModelPhase::demolished,
                    "component clipboard declared source row is inactive");
                registry = declared->second.registry_id;
                const auto& saved = phases.registries.at(*registry);
                phase = {{"membership_id", declared->second.membership_entity_id}, {"registry_id", *registry},
                    {"active_alternative", saved.active_alternative() ? Json(*saved.active_alternative()) : Json(nullptr)},
                    {"state", phase_name(state->second)},
                    {"membership_digest", source_digest(declared->second.membership_entity_id)},
                    {"registry_digest", source_digest(*registry)}};
            }
            phases.active(request.catalog_id, registry);
            const auto hosts = host_closure(entities, phases, *row, registry);
            auto instance = normalized(*row, request.independent_id);
            const auto expansion = decoded.expand(instance, selected_budget);
            require(!expansion.nodes.empty() && !expansion.profiles.empty(), "component clipboard source row has no genuine geometry");
            Entity external{request.independent_id, "assembly_instance", Json::object()};
            for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                if (catalog.properties.contains(key)) external.properties[key] = catalog.properties.at(key);
            external = encode_document_assembly_instance(external, {request.catalog_id, instance});
            result.roots.push_back(std::move(external));
            result.provenance.push_back({{"wire_owner_id", request.independent_id}, {"source_catalog_id", request.catalog_id},
                {"source_instance_id", request.instance_id}, {"source_catalog_digest", source_digest(request.catalog_id)},
                {"source_row", *raw_rows[index]}, {"source_hosts", hosts}, {"source_phase", std::move(phase)}});
        }
        result.roots.insert(result.roots.end(), independent_roots.begin(), independent_roots.end());
        result.catalogs = assembly_clipboard_dependencies(entities, result.roots);
        validate_assembly_component_clipboard_provenance(result.provenance);
        std::vector<Entity> wire = result.roots;
        for (const auto& [id, catalog] : result.catalogs) { (void)id; wire.push_back(catalog); }
        validate_assembly_component_clipboard_source_rows(result.provenance, wire);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid component clipboard capture: ") + error.what());
    }
}
} // namespace sketch
