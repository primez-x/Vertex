#include "sketch/annotation_entity_codec.hpp"

#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

const std::vector<SymbolDefinition>& catalog() {
    static const auto value = default_symbol_catalog();
    return value;
}

const nlohmann::json& state_json(const Entity& entity) {
    require(entity.type == kAnnotationEntityType,
            "entity is not an annotation state entity");
    require(entity.properties.is_object(),
            "annotation entity properties must be an object");
    require(entity.properties.contains("schema") &&
                entity.properties.contains("version") && entity.properties.contains("state"),
            "annotation entity has unknown or missing fields");
    require(entity.properties.at("schema") == "sketch.annotation_entity",
            "unsupported annotation entity schema");
    require(entity.properties.at("version").is_number_integer() &&
                (entity.properties.at("version") == 1 || entity.properties.at("version") == 2),
            "unsupported annotation entity version");
    const bool scoped = entity.properties.at("version") == 2;
    require(entity.properties.size() == (scoped ? (entity.properties.contains("level_id") ? 8 : 7) : 3),
            "annotation entity has unknown or missing fields");
    if(scoped) {
        const auto identifier = [&](const char* key) {
            require(entity.properties.contains(key) && entity.properties.at(key).is_string(),
                    "annotation drawing context requires string IDs");
            const auto& value=entity.properties.at(key).get_ref<const std::string&>();
            require(!value.empty() && value.size()<=256 &&
                value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:")==std::string::npos,
                "annotation drawing context ID must be a bounded stable token");
        };
        for(const auto key:{"property_id","building_id","floor_id","layer_id"})identifier(key);
        if(entity.properties.contains("level_id"))identifier("level_id");
    }
    require(entity.properties.at("state").is_object(),
            "annotation entity state must be an object");
    return entity.properties.at("state");
}

}  // namespace

Entity make_annotation_entity(std::string id, const AnnotationState& state,
                              std::optional<AnnotationEntityContext> context) {
    Entity entity = Entity::create(kAnnotationEntityType,
        {{"schema", "sketch.annotation_entity"}, {"version", 1},
         {"state", encode_annotation_state(state, catalog())}});
    entity.id = std::move(id);
    if(context) {
        entity.properties["version"]=2;
        entity.properties["property_id"]=context->property_id;
        entity.properties["building_id"]=context->building_id;
        entity.properties["floor_id"]=context->floor_id;
        entity.properties["layer_id"]=context->layer_id;
        if(context->level_id)entity.properties["level_id"]=*context->level_id;
        (void)state_json(entity);
    }
    return entity;
}

AnnotationState decode_annotation_entity(const Entity& entity) {
    return decode_annotation_state(state_json(entity), catalog());
}

void validate_annotation_entity(const Entity& entity) {
    (void)decode_annotation_entity(entity);
}

ApplyEntityChanges make_symbol_migration_command(const DocumentSnapshot& snapshot,
    std::string_view entity_id, std::string_view instance_id, std::string pinned_svg) {
    const auto found = snapshot.entities().find(entity_id);
    require(found != snapshot.entities().end(), "annotation entity not found");
    auto entity = found->second;
    const auto state = migrate_symbol_definition(decode_annotation_entity(entity), instance_id,
                                                 catalog(), std::move(pinned_svg));
    entity.properties["state"] = encode_annotation_state(state, catalog());
    return {snapshot.revision(), {EntityChange::upsert(std::move(entity))}, {},
            "Migrate symbol artwork revision"};
}

}  // namespace sketch
