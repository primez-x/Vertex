#include "sketch/sheet_view_entity_codec.hpp"

#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

const nlohmann::json& model_json(const Entity& entity) {
    require(entity.type == kSheetViewEntityType, "entity is not a sheet/view model entity");
    require(entity.properties.is_object(), "sheet/view entity properties must be an object");
    require(entity.properties.size() == 3 && entity.properties.contains("schema") &&
                entity.properties.contains("version") && entity.properties.contains("model"),
            "sheet/view entity has unknown or missing fields");
    require(entity.properties.at("schema") == "sketch.sheet_view_entity",
            "unsupported sheet/view entity schema");
    require(entity.properties.at("version").is_number_integer() &&
                entity.properties.at("version") == 1,
            "unsupported sheet/view entity version");
    require(entity.properties.at("model").is_object(),
            "sheet/view entity model must be an object");
    return entity.properties.at("model");
}

}  // namespace

Entity make_sheet_view_entity(std::string id, const SheetViewModel& model) {
    Entity entity = Entity::create(kSheetViewEntityType,
        {{"schema", "sketch.sheet_view_entity"}, {"version", 1}, {"model", model.to_json()}});
    entity.id = std::move(id);
    return entity;
}

SheetViewModel decode_sheet_view_entity(const Entity& entity) {
    return SheetViewModel::from_json(model_json(entity));
}

void validate_sheet_view_entity(const Entity& entity) {
    (void)decode_sheet_view_entity(entity);
}

}  // namespace sketch
