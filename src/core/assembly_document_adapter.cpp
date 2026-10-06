#include "sketch/assembly_document_adapter.hpp"
#include <algorithm>
#include <cctype>
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
ApplyEntityChanges upsert(const DocumentSnapshot& source, Entity entity,
    Revision expected, const char* label) {
    auto candidate = source.entities();
    candidate.insert_or_assign(entity.id, entity);
    validate_document_assembly_instances(candidate);
    return {expected, {EntityChange::upsert(std::move(entity))}, {}, label};
}
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
        entity.properties["model"] = AssemblyModel::create(std::move(retained_materials), std::move(types), {}).to_json();
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
    candidate.at(catalog_id).properties["model"]=catalog(found->second).with_type(std::move(replacement)).to_json();
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
    entity.properties["model"] = catalog(entity).with_type(std::move(replacement)).to_json();
    return upsert(source, std::move(entity), expected_revision, "Update assembly type");
}
ApplyEntityChanges independent_assembly_type_remove_command(const DocumentSnapshot& source,
    const std::string& catalog_id, const std::string& type_id, Revision expected_revision) {
    revision(source,expected_revision);
    const auto found=source.entities().find(catalog_id);
    require(found!=source.entities().end(),"assembly catalog removal target is missing");
    Entity entity=found->second;
    entity.properties["model"]=catalog(entity).without_type(type_id).to_json();
    return upsert(source,std::move(entity),expected_revision,"Remove assembly type");
}
} // namespace sketch
