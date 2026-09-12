#include "sketch/georeferencing_entity_codec.hpp"

#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

const nlohmann::json& model_json(const Entity& entity) {
    require(entity.type == kGeoreferencingEntityType,
            "entity is not a georeferencing entity");
    require(entity.properties.is_object(),
            "georeferencing entity properties must be an object");
    require(entity.properties.size() == 3 && entity.properties.contains("schema") &&
                entity.properties.contains("version") && entity.properties.contains("model"),
            "georeferencing entity has unknown or missing fields");
    require(entity.properties.at("schema") == "sketch.georeferencing_entity",
            "unsupported georeferencing entity schema");
    require(entity.properties.at("version").is_number_integer() &&
                entity.properties.at("version") == 1,
            "unsupported georeferencing entity version");
    require(entity.properties.at("model").is_object(),
            "georeferencing entity model must be an object");
    return entity.properties.at("model");
}

}  // namespace

Entity make_georeferencing_entity(std::string id,
                                   const GeoreferencingContract& contract) {
    Entity entity = Entity::create(
        kGeoreferencingEntityType,
        {{"schema", "sketch.georeferencing_entity"}, {"version", 1},
         {"model", contract.to_json()}});
    entity.id = std::move(id);
    return entity;
}

GeoreferencingContract decode_georeferencing_entity(const Entity& entity) {
    return GeoreferencingContract::from_json(model_json(entity));
}

void validate_georeferencing_entity(const Entity& entity) {
    (void)decode_georeferencing_entity(entity);
}

}  // namespace sketch
