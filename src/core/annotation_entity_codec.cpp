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
    require(entity.properties.size() == 3 && entity.properties.contains("schema") &&
                entity.properties.contains("version") && entity.properties.contains("state"),
            "annotation entity has unknown or missing fields");
    require(entity.properties.at("schema") == "sketch.annotation_entity",
            "unsupported annotation entity schema");
    require(entity.properties.at("version").is_number_integer() &&
                entity.properties.at("version") == 1,
            "unsupported annotation entity version");
    require(entity.properties.at("state").is_object(),
            "annotation entity state must be an object");
    return entity.properties.at("state");
}

}  // namespace

Entity make_annotation_entity(std::string id, const AnnotationState& state) {
    Entity entity = Entity::create(kAnnotationEntityType,
        {{"schema", "sketch.annotation_entity"}, {"version", 1},
         {"state", encode_annotation_state(state, catalog())}});
    entity.id = std::move(id);
    return entity;
}

AnnotationState decode_annotation_entity(const Entity& entity) {
    return decode_annotation_state(state_json(entity), catalog());
}

void validate_annotation_entity(const Entity& entity) {
    (void)decode_annotation_entity(entity);
}

}  // namespace sketch
