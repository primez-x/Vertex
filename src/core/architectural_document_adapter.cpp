#include "sketch/architectural_document_adapter.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {

using EntityState = std::map<std::string, Entity, std::less<>>;

EntityState copy_entities(const DocumentSnapshot& source) {
    return source.entities();
}

nlohmann::json properties_json(const std::map<std::string, std::string>& properties) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [key, value] : properties) result[key] = value;
    return result;
}

nlohmann::json transform_json(const ArchitecturalTransform& transform) {
    return { {"translation_m", {transform.x, transform.y, transform.z}},
             {"rotation_z_radians", transform.rotation_z_radians},
             {"uniform_scale", transform.scale} };
}

EntityState apply_operations(const DocumentSnapshot& source,
                             const ArchitecturalTransaction& transaction) {
    auto entities = copy_entities(source);
    const auto& initial = transaction.existing_ids();
    for (const auto& id : initial) {
        if (!entities.contains(id)) throw std::invalid_argument("architectural transaction source is stale");
    }
    for (const auto& operation : transaction.operations()) {
        switch (operation.action) {
        case ArchitecturalAction::create: {
            if (entities.contains(operation.object_id)) throw std::invalid_argument("architectural create ID already exists");
            if (!is_known_entity_type(operation.semantic_type))
                throw std::invalid_argument("architectural semantic type is not supported by Document");
            Entity created = Entity::create(operation.semantic_type, properties_json(operation.properties));
            created.id = operation.object_id;
            entities.emplace(created.id, std::move(created));
            break;
        }
        case ArchitecturalAction::select:
            if (!entities.contains(operation.object_id)) throw std::invalid_argument("architectural selection target is missing");
            break;
        case ArchitecturalAction::property_edit: {
            auto found = entities.find(operation.object_id);
            if (found == entities.end()) throw std::invalid_argument("architectural edit target is missing");
            for (const auto& [key, value] : operation.properties) found->second.properties[key] = value;
            break;
        }
        case ArchitecturalAction::transform: {
            auto found = entities.find(operation.object_id);
            if (found == entities.end() || !operation.transform)
                throw std::invalid_argument("architectural transform target is missing");
            found->second.properties["transform"] = transform_json(*operation.transform);
            break;
        }
        case ArchitecturalAction::duplicate: {
            auto found = entities.find(operation.object_id);
            if (found == entities.end()) throw std::invalid_argument("architectural duplicate source is missing");
            if (entities.contains(operation.duplicate_id)) throw std::invalid_argument("architectural duplicate ID already exists");
            auto copy = found->second;
            copy.id = operation.duplicate_id;
            entities.emplace(copy.id, std::move(copy));
            break;
        }
        case ArchitecturalAction::erase:
            if (entities.erase(operation.object_id) == 0) throw std::invalid_argument("architectural delete target is missing");
            break;
        }
    }
    return entities;
}

ApplyEntityChanges make_command(const DocumentSnapshot& source, const ArchitecturalTransaction& transaction,
                                Revision expected_revision) {
    const auto candidate = apply_operations(source, transaction);
    ApplyEntityChanges command;
    command.expected_revision = expected_revision;
    command.message = transaction.undo_label();
    std::set<std::string, std::less<>> ids;
    for (const auto& [id, entity] : source.entities()) ids.insert(id);
    for (const auto& [id, entity] : candidate) ids.insert(id);
    for (const auto& id : ids) {
        const auto before = source.entities().find(id);
        const auto after = candidate.find(id);
        if (before != source.entities().end() && after == candidate.end()) {
            command.entity_changes.push_back(EntityChange::erase(id));
        } else if (after != candidate.end() &&
                   (before == source.entities().end() || before->second != after->second)) {
            command.entity_changes.push_back(EntityChange::upsert(after->second));
        }
    }
    return command;
}

}  // namespace

DocumentSnapshot preview_architectural_transaction(const DocumentSnapshot& source,
                                                   const ArchitecturalTransaction& transaction) {
    const auto command = make_command(source, transaction, source.revision());
    if (command.entity_changes.empty()) return source;
    return Document::preview_command(source, Command{command});
}

Revision apply_architectural_transaction(Document& document,
                                         const ArchitecturalTransaction& transaction,
                                         Revision expected_revision) {
    const auto source = document.snapshot();
    const auto command = make_command(source, transaction, expected_revision);
    if (command.entity_changes.empty()) return document.revision();
    return document.apply(Command{command});
}

}  // namespace sketch
