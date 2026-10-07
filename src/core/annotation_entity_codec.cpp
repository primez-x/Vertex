#include "sketch/annotation_entity_codec.hpp"
#include "sketch/site_frame.hpp"

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
                (entity.properties.at("version") == 1 || entity.properties.at("version") == 2 ||
                 entity.properties.at("version") == 3),
            "unsupported annotation entity version");
    const bool framed = entity.properties.at("version") == 3;
    const bool has_context = entity.properties.contains("property_id") ||
        entity.properties.contains("building_id") || entity.properties.contains("floor_id") ||
        entity.properties.contains("layer_id") || entity.properties.contains("level_id");
    const bool scoped = entity.properties.at("version") == 2 || (framed && has_context);
    require(entity.properties.size() ==
                (scoped ? (entity.properties.contains("level_id") ? 8 : 7) : 3) + (framed ? 1 : 0),
            "annotation entity has unknown or missing fields");
    if(framed) {
        require(entity.properties.contains("presentation_frame"),
                "annotation entity v3 requires presentation_frame");
        (void)decode_presentation_frame(entity.properties.at("presentation_frame"));
    }
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
    const auto& encoded=state_json(entity);
    auto decoded=decode_annotation_state(encoded, catalog());
    if(entity.properties.at("version")==3) {
        // The owner frame applies to each child's own layer. A second frame
        // in child JSON cannot be silently ignored by the state decoder.
        for(const auto* collection:{"labels","symbols"})for(const auto& child:encoded.at(collection)) {
            require(!child.contains("presentation_frame") && !child.at("placement").contains("presentation_frame"),
                "annotation child presentation_frame is owned only by the framed annotation entity");
        }
    }
    return decoded;
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
    // Replace only the explicitly migrated artwork fields. Re-encoding the
    // whole state would discard opaque label/symbol/override siblings.
    const auto encoded = encode_annotation_state(state, catalog());
    auto& symbols = entity.properties["state"]["symbols"];
    const auto& migrated_symbols = encoded.at("symbols");
    for(std::size_t index = 0; index < symbols.size(); ++index) {
        if(symbols.at(index).at("id").get<std::string>() != instance_id) continue;
        for(const auto* key : {"definition", "pinned_svg"}) {
            if(migrated_symbols.at(index).contains(key))
                symbols.at(index)[key] = migrated_symbols.at(index).at(key);
            else symbols.at(index).erase(key);
        }
        break;
    }
    return {snapshot.revision(), {EntityChange::upsert(std::move(entity))}, {},
            "Migrate symbol artwork revision"};
}

}  // namespace sketch
