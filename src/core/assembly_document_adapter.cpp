#include "sketch/assembly_document_adapter.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/wall_semantics.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <numbers>
#include <stdexcept>
#include <set>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void identifier(const std::string& value) {
    require(!value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "assembly catalog/entity identity must be a valid document identifier");
}
void local_identifier(const std::string& value) {
    // Match AssemblyModel's local identity rules without imposing document-ID
    // characters/length on authored part, type, profile or embedded instance IDs.
    require(!value.empty() && !std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); }), "assembly identifier/name must not be blank");
}
struct ArchitecturalMaterialReferenceSite {
    // Absence identifies the root properties.material_assignment envelope.
    std::optional<std::size_t> layer_index;
    ArchitecturalMaterialSourceReference reference;
};
std::vector<ArchitecturalMaterialReferenceSite> architectural_material_reference_sites(const Entity& source) {
    const auto& properties = source.properties;
    require(properties.is_object(), "architectural material source properties must be an object");
    std::vector<ArchitecturalMaterialReferenceSite> result;
    if (properties.contains("material_assignment")) {
        static constexpr std::array<std::string_view, 11> roles{
            "wall", "opening", "room", "room_boundary", "slab", "roof", "stair", "railing", "column", "beam", "roof_join"};
        require(std::find(roles.begin(), roles.end(), source.type) != roles.end(),
            "material assignment requires an architectural object");
        const auto& assignment = properties.at("material_assignment");
        require(assignment.is_object() && assignment.contains("version") &&
            assignment.at("version").is_number_integer() && assignment.at("version") == 1 &&
            assignment.contains("catalog_id") && assignment.at("catalog_id").is_string() &&
            assignment.contains("material_id") && assignment.at("material_id").is_string(),
            "architectural material assignment must be version 1 with string identities");
        ArchitecturalMaterialSourceReference reference{assignment.at("catalog_id").get<std::string>(),
            assignment.at("material_id").get<std::string>()};
        identifier(reference.catalog_id);
        local_identifier(reference.material_id);
        result.push_back({std::nullopt, std::move(reference)});
    }
    if ((source.type == "wall" || source.type == "slab") && properties.contains("layers")) {
        const auto& raw_layers = properties.at("layers");
        // The codecs enforce the same cap after decoding; charge it before their
        // reserve/iteration as this inventory may inspect detached input.
        require(raw_layers.is_array() && raw_layers.size() <= 1024,
            "architectural material source layer count exceeds the supported limit or is not an array");
        const auto canonical = properties.find("thickness_m");
        const auto legacy = properties.find("thickness");
        const auto* thickness = canonical != properties.end() ? &canonical.value() :
            (legacy != properties.end() ? &legacy.value() : nullptr);
        require(thickness != nullptr && thickness->is_number(),
            "architectural material source layers require a numeric thickness_m");
        const auto append = [&](const auto& layers) {
            for (std::size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].material) {
                    const auto& material = *layers[i].material;
                    result.push_back({i, {material.catalog_id, material.material_id}});
                }
            }
        };
        if (source.type == "wall") append(parse_wall_layers(raw_layers, thickness->get<double>()));
        else append(parse_slab_layers(raw_layers, thickness->get<double>()));
    }
    return result;
}
void validate_profile_presentation_identity(const AssemblyProfilePresentationIdentity& identity) {
    identifier(identity.catalog_id);
    local_identifier(identity.instance_id);
    if (identity.document_entity_id) {
        identifier(*identity.document_entity_id);
        require(identity.instance_id == *identity.document_entity_id,
            "assembly profile instance identity differs from its document root");
    }
    require(identity.part_path.size() < 32, "assembly graph depth budget exceeded");
    for (const auto& part : identity.part_path) local_identifier(part);
    local_identifier(identity.type_id);
    local_identifier(identity.profile_id);
}
AssemblyModel catalog(const Entity& entity) {
    require(entity.type == "assembly_model", "assembly catalog reference has the wrong entity type");
    require(entity.properties.is_object() && entity.properties.contains("model") &&
        entity.properties.at("model").is_object(), "assembly catalog requires a model object");
    return AssemblyModel::from_json(entity.properties.at("model"));
}
void revision(const DocumentSnapshot& source, Revision expected) {
    if (source.revision() != expected)
        throw DocumentError(DocumentErrorCode::stale_revision, "assembly command revision is stale");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, "assembly command source is read only");
}
const ArchitecturalGroupTransform& embedded_group_transform(const EmbeddedAssemblyGroupTarget& target,
    const ArchitecturalGroupTransform& shared) {
    const auto& transform = target.transform ? *target.transform : shared;
    const auto finite_point = [](const auto& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    require(finite_point(transform.pivot) && finite_point(transform.offset) &&
        std::isfinite(transform.rotation_z_radians) && std::isfinite(transform.scale) &&
        transform.scale > 0.0, "assembly group transform must be finite with a positive scale");
    return transform;
}
ApplyEntityChanges upsert(const DocumentSnapshot& source, Entity entity,
    Revision expected, const char* label) {
    auto candidate = source.entities();
    candidate.insert_or_assign(entity.id, entity);
    validate_document_assembly_instances(candidate);
    return {expected, {EntityChange::upsert(std::move(entity))}, {}, label};
}
}

std::vector<ArchitecturalMaterialSourceReference> architectural_material_source_refs(const Entity& source) {
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& site : architectural_material_reference_sites(source))
        unique.emplace(site.reference.catalog_id, site.reference.material_id);
    std::vector<ArchitecturalMaterialSourceReference> result;
    result.reserve(unique.size());
    for (const auto& [catalog_id, material_id] : unique) result.push_back({catalog_id, material_id});
    return result;
}
Entity remap_architectural_material_source_refs(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& catalog_mapping) {
    const auto sites = architectural_material_reference_sites(source);
    // Validate all reached mappings before copying or patching any raw data.
    for (const auto& site : sites) {
        const auto destination = catalog_mapping.find(site.reference.catalog_id);
        require(destination != catalog_mapping.end(), "architectural material catalog mapping is missing");
        identifier(destination->second);
    }
    Entity result = source;
    for (const auto& site : sites) {
        auto& assignment = site.layer_index
            ? result.properties.at("layers").at(*site.layer_index).at("material_assignment")
            : result.properties.at("material_assignment");
        assignment.at("catalog_id") = catalog_mapping.at(site.reference.catalog_id);
    }
    return result;
}

nlohmann::json encode_assembly_profile_presentation_identity(
    const AssemblyProfilePresentationIdentity& identity) {
    validate_profile_presentation_identity(identity);
    return {{"version", 1},
        {"origin", identity.document_entity_id ? "document_instance" : "embedded_catalog"},
        {"catalog_id", identity.catalog_id}, {"instance_id", identity.instance_id},
        {"document_entity_id", identity.document_entity_id
            ? nlohmann::json(*identity.document_entity_id) : nlohmann::json(nullptr)},
        {"part_path", identity.part_path}, {"type_id", identity.type_id},
        {"profile_id", identity.profile_id}};
}
AssemblyProfilePresentationIdentity decode_assembly_profile_presentation_identity(const nlohmann::json& value) {
    require(value.is_object(), "assembly profile presentation identity must be an object");
    require(value.contains("version") && value.at("version").is_number_integer() && value.at("version") == 1,
        "unsupported assembly profile presentation identity version");
    for (const auto* key : {"origin", "catalog_id", "instance_id", "type_id", "profile_id"})
        require(value.contains(key) && value.at(key).is_string(),
            "assembly profile presentation identity requires string identities and origin");
    require(value.contains("part_path") && value.at("part_path").is_array() &&
        value.at("part_path").size() < 32, "invalid assembly profile presentation path");
    for (const auto& part : value.at("part_path"))
        require(part.is_string(), "assembly profile presentation path identities must be strings");
    require(value.contains("document_entity_id"), "assembly profile presentation identity requires document root field");
    const auto& root = value.at("document_entity_id");
    const auto& origin = value.at("origin");
    require((origin == "document_instance" && root.is_string()) ||
        (origin == "embedded_catalog" && root.is_null()),
        "assembly profile presentation origin disagrees with its document root");
    AssemblyProfilePresentationIdentity result{value.at("catalog_id").get<std::string>(),
        value.at("instance_id").get<std::string>(), std::nullopt,
        value.at("part_path").get<std::vector<std::string>>(),
        value.at("type_id").get<std::string>(), value.at("profile_id").get<std::string>()};
    if (!root.is_null()) result.document_entity_id = root.get<std::string>();
    validate_profile_presentation_identity(result);
    return result;
}
nlohmann::json assembly_profile_presentation_key(const std::string& catalog_id,
    const AssemblyInstance& instance, const AssemblyExpandedProfile& profile,
    const std::optional<std::string>& document_entity_id) {
    return encode_assembly_profile_presentation_identity({catalog_id, instance.id,
        document_entity_id, profile.part_path, profile.type_id, profile.profile.id});
}

EmbeddedAssemblyPresentationIds embedded_assembly_presentation_ids(const AssemblyDocumentEntities& entities) {
    EmbeddedAssemblyPresentationIds aliases;
    std::map<std::string, std::size_t, std::less<>> alias_counts;
    std::set<std::string, std::less<>> occupied;
    for (const auto& [id, entity] : entities) {
        require(id == entity.id, "document map identity differs from entity identity");
        occupied.insert(id);
        if (entity.type == kAnnotationEntityType) {
            require(entity.properties.is_object() && entity.properties.contains("state") &&
                entity.properties.at("state").is_object(), "annotation requires a state object");
            const auto& state = entity.properties.at("state");
            for (const auto* key : {"labels", "symbols"}) {
                require(state.contains(key) && state.at(key).is_array(), "annotation requires child arrays");
                for (const auto& child : state.at(key)) {
                    require(child.is_object() && child.contains("id") && child.at("id").is_string(),
                        "annotation child requires an identity");
                    const auto child_id = child.at("id").get<std::string>();
                    local_identifier(child_id);
                    occupied.insert(child_id);
                }
            }
        } else if (entity.type == "assembly_model") {
            identifier(id);
            require(entity.properties.is_object() && entity.properties.contains("model") &&
                entity.properties.at("model").is_object(), "assembly catalog requires a model object");
            const auto& model = entity.properties.at("model");
            require(model.contains("instances") && model.at("instances").is_array(),
                "assembly catalog requires an instances array");
            for (const auto& instance : model.at("instances")) {
                require(instance.is_object() && instance.contains("id") && instance.at("id").is_string(),
                    "embedded assembly requires an instance identity");
                const auto instance_id = instance.at("id").get<std::string>();
                local_identifier(instance_id);
                const auto alias = id + ":instance:" + instance_id;
                require(aliases.emplace(std::pair{id, instance_id}, alias).second,
                    "duplicate embedded assembly instance identity");
                ++alias_counts[alias];
            }
        }
    }
    // Reserve even ambiguous raw aliases so escaped IDs cannot claim another
    // embedded instance's historical spelling. Framed payloads are injective;
    // leading '@' escapes also cover arbitrary authored annotation child IDs.
    auto reserved = occupied;
    for (const auto& [alias, count] : alias_counts) { (void)count; reserved.insert(alias); }
    for (auto& [binding, alias] : aliases) {
        if (alias_counts.at(alias) == 1 && !occupied.contains(alias)) continue;
        auto qualified = "@assembly-instance:" + nlohmann::json::array({binding.first, binding.second}).dump();
        while (reserved.contains(qualified)) qualified.insert(qualified.begin(), '@');
        reserved.insert(qualified);
        alias = std::move(qualified);
    }
    return aliases;
}
std::string embedded_assembly_presentation_id(const AssemblyDocumentEntities& entities,
    const std::string& catalog_id, const std::string& instance_id) {
    identifier(catalog_id);
    local_identifier(instance_id);
    const auto aliases = embedded_assembly_presentation_ids(entities);
    const auto found = aliases.find({catalog_id, instance_id});
    require(found != aliases.end(), "embedded assembly presentation references a missing instance");
    return found->second;
}
std::optional<AssemblyDocumentInstance> resolve_embedded_assembly_presentation(
    const AssemblyDocumentEntities& entities, std::string_view render_id) {
    if (render_id.empty() || entities.contains(render_id)) return std::nullopt;
    const auto aliases = embedded_assembly_presentation_ids(entities);
    const auto found = std::find_if(aliases.begin(), aliases.end(),
        [&](const auto& value) { return value.second == render_id; });
    if (found == aliases.end()) return std::nullopt;
    const auto model = catalog(entities.at(found->first.first));
    const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
        [&](const auto& value) { return value.id == found->first.second; });
    require(instance != model.instances().end(), "embedded assembly presentation source instance is missing");
    return AssemblyDocumentInstance{found->first.first, *instance};
}

AssemblyDocumentInstance decode_document_assembly_instance(const Entity& entity) {
    require(entity.type == "assembly_instance", "expected an assembly_instance entity");
    identifier(entity.id);
    const auto& p = entity.properties;
    require(p.is_object(), "assembly instance properties must be an object");
    require(p.contains("version") && p.at("version").is_number_integer() && p.at("version") == 1,
        "unsupported assembly instance entity version");
    require(p.contains("form") && p.at("form").is_string() && p.at("form") == "independent_assembly_instance",
        "unsupported assembly instance entity form");
    require(p.contains("assembly_catalog_id") && p.at("assembly_catalog_id").is_string(),
        "assembly instance requires assembly_catalog_id");
    require(p.contains("instance") && p.at("instance").is_object(), "assembly instance requires an instance object");
    // Historical placement fields are owned forbidden aliases, not opaque data.
    for (const auto* key : {"host", "host_entity_id", "placement", "root_transform", "type_id"})
        require(!p.contains(key), "independent assembly instance has a forbidden placement/type alias");
    AssemblyDocumentInstance result{p.at("assembly_catalog_id").get<std::string>(),
        decode_assembly_instance(p.at("instance"))};
    identifier(result.assembly_catalog_id);
    require(result.instance.id == entity.id, "assembly instance identity differs from its document entity");
    require(result.instance.root_transform.has_value() && !result.instance.placement,
        "independent assembly instance requires root_transform and forbids host placement");
    return result;
}
Entity encode_document_assembly_instance(const Entity& source, const AssemblyDocumentInstance& value) {
    identifier(source.id);
    require(source.type == "assembly_instance", "expected an assembly_instance entity");
    require(source.properties.is_object(), "assembly instance properties must be an object");
    identifier(value.assembly_catalog_id);
    require(value.instance.id == source.id, "assembly instance identity differs from its document entity");
    require(value.instance.root_transform.has_value() && !value.instance.placement,
        "independent assembly instance requires root_transform and forbids host placement");
    Entity result = source;
    result.properties["version"] = 1;
    result.properties["form"] = "independent_assembly_instance";
    result.properties["assembly_catalog_id"] = value.assembly_catalog_id;
    result.properties["instance"] = encode_assembly_instance(value.instance);
    if (source.properties.contains("instance") && source.properties.at("instance").is_object() &&
        source.properties.at("instance").value("schema", nlohmann::json{}) == "sketch.assembly-instance.v2")
        result.properties.at("instance")["schema"] = "sketch.assembly-instance.v2";
    (void)decode_document_assembly_instance(result);
    return result;
}
AssemblyExpansion expand_document_assembly_instance(const Entity& entity,
    const AssemblyDocumentEntities& entities, AssemblyExpansionBudget& budget) {
    const auto value = decode_document_assembly_instance(entity);
    const auto found = entities.find(value.assembly_catalog_id);
    require(found != entities.end(), "assembly instance references a missing catalog");
    return catalog(found->second).expand(value.instance, budget);
}
std::map<std::string, AssemblyExpansion, std::less<>> expand_document_assembly_instances(
    const AssemblyDocumentEntities& entities, AssemblyExpansionBudget& budget) {
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    for (const auto& [id, entity] : entities) {
        require(id == entity.id, "document map identity differs from entity identity");
        if (entity.type == "assembly_model") catalogs.emplace(id, catalog(entity));
    }
    auto candidate_budget = budget;
    // Legacy catalog instances still participate in the document-wide limits
    // and declared totals, even when they carry only a host placement.
    for (const auto& [id, model] : catalogs) {
        (void)id;
        for (const auto& instance : model.instances()) (void)model.expand(instance, candidate_budget);
    }
    std::map<std::string, AssemblyExpansion, std::less<>> result;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "assembly_instance") continue;
        const auto value = decode_document_assembly_instance(entity);
        const auto found = catalogs.find(value.assembly_catalog_id);
        require(found != catalogs.end(), "assembly instance references a missing catalog");
        result.emplace(id, found->second.expand(value.instance, candidate_budget));
    }
    budget = std::move(candidate_budget);
    return result;
}
void validate_document_assembly_instances(const AssemblyDocumentEntities& entities) {
    AssemblyExpansionBudget budget;
    (void)expand_document_assembly_instances(entities, budget);
}
AssemblyDocumentEntities assembly_clipboard_dependencies(const DocumentSnapshot& source,
    const std::vector<std::string>& root_entity_ids) {
    AssemblyExpansionBudget source_budget;
    (void)expand_document_assembly_instances(source.entities(), source_budget);
    std::map<std::string, std::vector<AssemblyInstance>, std::less<>> roots;
    std::set<std::string> selected;
    AssemblyDocumentEntities closure;
    for (const auto& id : root_entity_ids) {
        require(selected.insert(id).second, "duplicate assembly clipboard root");
        const auto found = source.entities().find(id);
        require(found != source.entities().end(), "assembly clipboard root is missing");
        const auto value = decode_document_assembly_instance(found->second);
        roots[value.assembly_catalog_id].push_back(value.instance);
        closure.emplace(id, found->second);
    }
    AssemblyDocumentEntities result;
    for (const auto& [catalog_id, instances] : roots) {
        const auto& source_entity = source.entities().at(catalog_id);
        const auto model = catalog(source_entity);
        std::set<std::string> type_ids, material_ids;
        std::vector<std::string> pending;
        auto materials = [&](const auto& bindings) {
            for (const auto& [slot, id] : bindings) { (void)slot; material_ids.insert(id); }
        };
        for (const auto& instance : instances) {
            pending.push_back(instance.type_id);
            materials(instance.material_overrides);
            for (const auto& override : instance.nested_overrides) materials(override.material_overrides);
        }
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (!type_ids.insert(id).second) continue;
            const auto found = std::find_if(model.types().begin(), model.types().end(),
                [&](const auto& type) { return type.id == id; });
            require(found != model.types().end(), "assembly clipboard type is missing");
            materials(found->materials);
            for (const auto& part : found->parts) {
                pending.push_back(part.type_id);
                materials(part.material_overrides);
            }
        }
        std::vector<AssemblyType> types;
        std::vector<AssemblyMaterial> retained_materials;
        for (const auto& type : model.types()) if (type_ids.contains(type.id)) types.push_back(type);
        for (const auto& material : model.materials())
            if (material_ids.contains(material.id)) retained_materials.push_back(material);
        Entity entity = source_entity;
        entity.properties["model"] = retain_assembly_catalog_dialect(source_entity.properties.at("model"),
            AssemblyModel::create(std::move(retained_materials), std::move(types), {}).to_json());
        result.emplace(catalog_id, entity);
        closure.emplace(catalog_id, std::move(entity));
    }
    AssemblyExpansionBudget closure_budget;
    (void)expand_document_assembly_instances(closure, closure_budget);
    return result;
}
Entity remap_independent_assembly_instance(const Entity& source,
    const std::map<std::string, std::string>& identity_mapping) {
    auto value = decode_document_assembly_instance(source);
    const auto root = identity_mapping.find(source.id);
    const auto catalog_id = identity_mapping.find(value.assembly_catalog_id);
    require(root != identity_mapping.end() && catalog_id != identity_mapping.end(),
        "assembly clipboard requires root and catalog identity mappings");
    identifier(root->second); identifier(catalog_id->second);
    require(root->second != catalog_id->second, "assembly root and catalog mapping collide");
    Entity result = source;
    result.id = root->second;
    value.instance.id = result.id;
    value.assembly_catalog_id = catalog_id->second;
    return encode_document_assembly_instance(result, value);
}
std::vector<AssemblyDocumentTypeUpdateImpact> preview_document_assembly_type_update(
    const AssemblyDocumentEntities& entities, const std::string& catalog_id, AssemblyType replacement) {
    const auto found=entities.find(catalog_id);
    require(found!=entities.end(),"assembly catalog update target is missing");
    const auto type_id=replacement.id;
    auto candidate=entities;
    candidate.at(catalog_id).properties["model"]=retain_assembly_catalog_dialect(found->second.properties.at("model"),
        catalog(found->second).with_type(std::move(replacement)).to_json());
    AssemblyExpansionBudget before_budget,after_budget;
    const auto before=expand_document_assembly_instances(entities,before_budget);
    const auto after=expand_document_assembly_instances(candidate,after_budget);
    std::vector<AssemblyDocumentTypeUpdateImpact> result;
    for(const auto& [id,expansion]:before) {
        const auto value=decode_document_assembly_instance(entities.at(id));
        if(value.assembly_catalog_id==catalog_id && std::any_of(expansion.nodes.begin(),expansion.nodes.end(),
            [&](const auto& node){return node.type_id==type_id;}))
            result.push_back({id,expansion,after.at(id),value});
    }
    return result;
}
ApplyEntityChanges assembly_instance_create_command(const DocumentSnapshot& source, Entity entity,
    const AssemblyDocumentInstance& value, Revision expected_revision) {
    revision(source, expected_revision);
    require(!source.entities().contains(entity.id), "assembly instance identity already exists");
    return upsert(source, encode_document_assembly_instance(entity, value), expected_revision, "Create assembly instance");
}
ApplyEntityChanges assembly_instance_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const AssemblyDocumentInstance& value, Revision expected_revision) {
    revision(source, expected_revision);
    const auto found = source.entities().find(entity_id);
    require(found != source.entities().end(), "assembly instance update target is missing");
    (void)decode_document_assembly_instance(found->second);
    return upsert(source, encode_document_assembly_instance(found->second, value), expected_revision, "Update assembly instance");
}
ApplyEntityChanges assembly_instance_remove_command(const DocumentSnapshot& source,
    const std::string& entity_id, Revision expected_revision) {
    revision(source, expected_revision);
    const auto found = source.entities().find(entity_id);
    require(found != source.entities().end(), "assembly instance removal target is missing");
    (void)decode_document_assembly_instance(found->second);
    auto candidate = source.entities();
    candidate.erase(entity_id);
    validate_document_assembly_instances(candidate);
    return {expected_revision, {EntityChange::erase(entity_id)}, {}, "Remove assembly instance"};
}
ApplyEntityChanges independent_assembly_type_update_command(const DocumentSnapshot& source,
    const std::string& catalog_id, AssemblyType replacement, Revision expected_revision) {
    revision(source, expected_revision);
    const auto found = source.entities().find(catalog_id);
    require(found != source.entities().end(), "assembly catalog update target is missing");
    Entity entity = found->second;
    entity.properties["model"] = retain_assembly_catalog_dialect(found->second.properties.at("model"),
        catalog(entity).with_type(std::move(replacement)).to_json());
    return upsert(source, std::move(entity), expected_revision, "Update assembly type");
}
ApplyEntityChanges independent_assembly_type_remove_command(const DocumentSnapshot& source,
    const std::string& catalog_id, const std::string& type_id, Revision expected_revision) {
    revision(source,expected_revision);
    const auto found=source.entities().find(catalog_id);
    require(found!=source.entities().end(),"assembly catalog removal target is missing");
    Entity entity=found->second;
    entity.properties["model"]=retain_assembly_catalog_dialect(found->second.properties.at("model"),
        catalog(entity).without_type(type_id).to_json());
    return upsert(source,std::move(entity),expected_revision,"Remove assembly type");
}
ApplyEntityChanges embedded_assembly_group_transform_command(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& transform, Revision expected_revision) {
    revision(source, expected_revision);
    require(!targets.empty() && targets.size() <= maximum_architectural_group_targets,
        "an embedded assembly group requires between 1 and 1000 roots");
    const auto finite_point = [](const auto& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    using Identity = std::pair<std::string, std::string>;
    std::set<Identity> selected, copies;
    std::map<std::string, AssemblyModel, std::less<>> models;
    for (const auto& target : targets) {
        (void)embedded_group_transform(target, transform);
        identifier(target.catalog_id);
        identifier(target.instance_id);
        require(selected.emplace(target.catalog_id, target.instance_id).second,
            "duplicate embedded assembly group root");
        const auto found = source.entities().find(target.catalog_id);
        require(found != source.entities().end() && found->second.id == target.catalog_id,
            "embedded assembly group catalog is missing or has inconsistent identity");
        if (!models.contains(target.catalog_id))
            models.emplace(target.catalog_id, catalog(found->second));
        const auto& model = models.at(target.catalog_id);
        require(std::any_of(model.instances().begin(), model.instances().end(),
            [&](const auto& instance) { return instance.id == target.instance_id; }),
            "embedded assembly group instance is missing from its catalog");
        if (target.copy_instance_id) {
            identifier(*target.copy_instance_id);
            require(copies.emplace(target.catalog_id, *target.copy_instance_id).second &&
                std::none_of(model.instances().begin(), model.instances().end(),
                    [&](const auto& instance) { return instance.id == *target.copy_instance_id; }),
                "embedded assembly copy identity already exists");
        }
    }
    // Includes all embedded and independent roots, not just selected profiles.
    // Identity gestures must pass the same source budgets and descriptors.
    validate_document_assembly_instances(source.entities());
    struct Root {
        std::size_t raw_index;
        AssemblyTransform pose;
    };
    std::vector<Root> roots;
    roots.reserve(targets.size());
    AssemblyExpansionBudget selected_budget;
    for (const auto& target : targets) {
        const auto& model = models.at(target.catalog_id);
        const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
            [&](const auto& value) { return value.id == target.instance_id; });
        const auto expansion = model.expand(*instance, selected_budget);
        require(!expansion.profiles.empty() && !expansion.nodes.empty(),
            "embedded assembly group requires genuine geometric profile expansion");
        const auto& raw_instances = source.entities().at(target.catalog_id).properties.at("model").at("instances");
        const auto raw = std::find_if(raw_instances.begin(), raw_instances.end(),
            [&](const auto& value) { return value.at("id") == target.instance_id; });
        require(raw != raw_instances.end(), "embedded assembly source record is missing");
        roots.push_back({static_cast<std::size_t>(std::distance(raw_instances.begin(), raw)),
            expansion.nodes.front().transform});
    }

    const auto whole_turn = 2.0 * std::numbers::pi;
    std::map<std::string, Entity, std::less<>> changed;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        const auto& operation = embedded_group_transform(target, transform);
        const bool reflected = operation.flip_horizontal != operation.flip_vertical;
        // Two flips are a proper half-turn. Normalize before addition so even a
        // finite, very large angle cannot overflow, and exact whole turns vanish.
        const auto angle = std::remainder(std::remainder(operation.rotation_z_radians, whole_turn) +
            (operation.flip_horizontal && operation.flip_vertical ? std::numbers::pi : 0.0), whole_turn);
        const bool identity = angle == 0.0 && !reflected && operation.scale == 1.0 &&
            operation.offset.x == 0.0 && operation.offset.y == 0.0 && operation.offset.z == 0.0;
        const auto cosine = std::abs(angle) == std::numbers::pi ? -1.0 : std::cos(angle);
        const auto sine = std::abs(angle) == std::numbers::pi ? 0.0 : std::sin(angle);
        const auto hx = reflected && operation.flip_horizontal ? -1.0 : 1.0;
        const auto hy = reflected && operation.flip_vertical ? -1.0 : 1.0;
        const auto moved_pose = [&](AssemblyTransform root) {
            if (angle == 0.0 && !reflected && operation.scale == 1.0) {
                root.translation_m.x += operation.offset.x;
                root.translation_m.y += operation.offset.y;
                root.translation_m.z += operation.offset.z;
            } else {
                // Subtract this root's captured pivot before scale/rotation;
                // no enormous affine translation is subtracted from source pose.
                const auto dx = root.translation_m.x - operation.pivot.x;
                const auto dy = root.translation_m.y - operation.pivot.y;
                const auto dz = root.translation_m.z - operation.pivot.z;
                root.translation_m = {
                    operation.pivot.x + hx * operation.scale * (cosine * dx - sine * dy) + operation.offset.x,
                    operation.pivot.y + hy * operation.scale * (sine * dx + cosine * dy) + operation.offset.y,
                    operation.pivot.z + operation.scale * dz + operation.offset.z};
            }
            if (angle != 0.0 || reflected) {
                auto heading = std::remainder(std::remainder(root.rotation_radians, whole_turn) + angle, whole_turn);
                if (reflected) heading = operation.flip_horizontal ? std::numbers::pi - heading : -heading;
                root.rotation_radians = std::remainder(heading, whole_turn);
            }
            root.scale *= operation.scale;
            root.mirrored_y = root.mirrored_y != reflected;
            require(finite_point(root.translation_m) && std::isfinite(root.rotation_radians) &&
                std::isfinite(root.scale) && root.scale > 0.0,
                "embedded assembly group pose exceeds the supported transform range");
            return root;
        };

        if (identity && !target.copy_instance_id) continue;
        const auto& before = source.entities().at(target.catalog_id);
        auto [entry, inserted] = changed.try_emplace(target.catalog_id, before);
        (void)inserted;
        auto& instances = entry->second.properties.at("model").at("instances");
        // Always clone the captured original, even if another selected record
        // in this same catalog has already been transformed in the candidate.
        auto raw = before.properties.at("model").at("instances").at(roots[i].raw_index);
        if (target.copy_instance_id) raw["id"] = *target.copy_instance_id;
        if (!identity) {
            const auto pose = moved_pose(roots[i].pose);
            raw.erase("placement");
            // Patch only owned pose fields, retaining an existing explicit
            // false parity field and all raw override/nested-override records.
            auto& encoded = raw["root_transform"];
            if (!encoded.is_object()) {
                encoded = encode_assembly_transform(pose);
            } else {
                const auto& previous = roots[i].pose;
                auto& translation = encoded.at("translation_m");
                if (pose.translation_m.x != previous.translation_m.x) translation[0] = pose.translation_m.x;
                if (pose.translation_m.y != previous.translation_m.y) translation[1] = pose.translation_m.y;
                if (pose.translation_m.z != previous.translation_m.z) translation[2] = pose.translation_m.z;
                if (pose.rotation_radians != previous.rotation_radians) encoded["rotation_radians"] = pose.rotation_radians;
                if (pose.scale != previous.scale) encoded["scale"] = pose.scale;
                if (pose.mirrored_y != previous.mirrored_y) encoded["mirrored_y"] = pose.mirrored_y;
                if (pose.vertical_scale != previous.vertical_scale) encoded["vertical_scale"] = pose.vertical_scale;
            }
            // A retained v7 row may omit both envelope slots. Materializing its
            // selected root must supply the pair without changing other rows.
            if (!raw.contains("nested_overrides")) raw["nested_overrides"] = nlohmann::json::array();
        }
        if (target.copy_instance_id) instances.push_back(std::move(raw));
        else instances.at(roots[i].raw_index) = std::move(raw);
    }
    ApplyEntityChanges command{expected_revision, {}, {},
        copies.empty() ? "Transform embedded assembly group" : "Copy/transform embedded assembly group"};
    for (auto& [id, entity] : changed) {
        (void)id;
        command.entity_changes.push_back(EntityChange::upsert(std::move(entity)));
    }
    if (command.entity_changes.empty()) (void)Document::fork(source);
    else (void)Document::preview_command(source, command);
    return command;
}
ApplyEntityChanges embedded_assembly_group_copy_command(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& transform, Revision expected_revision) {
    revision(source, expected_revision);
    require(!targets.empty() && targets.size() <= maximum_architectural_group_targets,
        "an embedded assembly copy requires between 1 and 1000 roots");
    std::set<std::string, std::less<>> fresh_ids;
    std::vector<EmbeddedAssemblyGroupTarget> moving;
    moving.reserve(targets.size());
    for (const auto& target : targets) {
        require(target.copy_instance_id.has_value(), "an embedded assembly copy requires a fresh document identity");
        identifier(*target.copy_instance_id);
        require(!source.entities().contains(*target.copy_instance_id) && fresh_ids.insert(*target.copy_instance_id).second,
            "embedded assembly copy document identity already exists");
        moving.push_back({target.catalog_id, target.instance_id, std::nullopt, target.transform});
    }
    // Derive exact poses through the same qualified per-root command. Its
    // existing-catalog upserts are detached input only, never published here.
    const auto movement = embedded_assembly_group_transform_command(source, moving, transform, expected_revision);
    std::map<std::string, const Entity*, std::less<>> moved_catalogs;
    for (const auto& change : movement.entity_changes) {
        require(change.kind == EntityChangeKind::upsert && change.entity.type == "assembly_model" &&
            moved_catalogs.emplace(change.entity.id, &change.entity).second,
            "embedded assembly copy has conflicting catalog consequences");
    }
    for (const auto& [id, entity] : source.entities()) {
        if (entity.type == kAnnotationEntityType) {
            const auto state = decode_annotation_entity(entity);
            for (const auto& child : state.labels)
                require(!fresh_ids.contains(child.id), "embedded assembly copy identity collides with an annotation");
            for (const auto& child : state.symbols)
                require(!fresh_ids.contains(child.id), "embedded assembly copy identity collides with a component");
        } else if (entity.type == "assembly_model") {
            for (const auto& instance : entity.properties.at("model").at("instances"))
                require(!fresh_ids.contains(id + ":instance:" + instance.at("id").get<std::string>()),
                    "embedded assembly copy identity collides with a catalog instance");
        }
    }
    ApplyEntityChanges command{expected_revision, {}, {}, "Copy embedded assemblies as independent objects"};
    AssemblyExpansionBudget budget;
    std::map<std::string, AssemblyModel, std::less<>> proposed_models;
    for (const auto& target : targets) {
        const auto& original = source.entities().at(target.catalog_id);
        const auto found_catalog = moved_catalogs.find(target.catalog_id);
        const auto& proposed = found_catalog == moved_catalogs.end() ? original : *found_catalog->second;
        if (!proposed_models.contains(target.catalog_id))
            proposed_models.emplace(target.catalog_id, catalog(proposed));
        const auto& model = proposed_models.at(target.catalog_id);
        const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
            [&](const auto& value) { return value.id == target.instance_id; });
        require(instance != model.instances().end(), "embedded assembly copy lost its qualified source instance");
        const auto expansion = model.expand(*instance, budget);
        require(!expansion.profiles.empty() && !expansion.nodes.empty(),
            "embedded assembly copy requires genuine geometric profile expansion");
        const auto& rows = proposed.properties.at("model").at("instances");
        const auto row = std::find_if(rows.begin(), rows.end(),
            [&](const auto& value) { return value.at("id") == target.instance_id; });
        require(row != rows.end(), "embedded assembly copy lost its raw source record");
        auto raw = *row;
        const auto catalog_schema = proposed.properties.at("model").value("schema", nlohmann::json{});
        raw["schema"] = catalog_schema == "sketch.assemblies.v6" || catalog_schema == "sketch.assemblies.v7"
            ? "sketch.assembly-instance.v2" : "sketch.assembly-instance.v1";
        raw["id"] = *target.copy_instance_id;
        if (raw.contains("placement") || !raw.contains("root_transform") || !raw.at("root_transform").is_object())
            raw["root_transform"] = encode_assembly_transform(expansion.nodes.front().transform);
        if (!raw.contains("nested_overrides")) raw["nested_overrides"] = nlohmann::json::array();
        raw.erase("placement");
        Entity copy = original;
        copy.id = *target.copy_instance_id;
        copy.type = "assembly_instance";
        copy.properties.erase("model");
        copy.properties["version"] = 1;
        copy.properties["form"] = "independent_assembly_instance";
        copy.properties["assembly_catalog_id"] = target.catalog_id;
        copy.properties["instance"] = std::move(raw);
        // Embedded profile poses are already world-authored. Materializing a
        // root must not apply its catalog's optional Site/building frame again.
        copy.properties["presentation_frame"] = {{"version", 1}, {"mode", "world"}};
        (void)decode_document_assembly_instance(copy);
        command.entity_changes.push_back(EntityChange::upsert(std::move(copy)));
    }
    (void)Document::preview_command(source, command);
    return command;
}
} // namespace sketch
