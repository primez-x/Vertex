#include "sketch/opening_host_geometry.hpp"

#include "sketch/architecture.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <stdexcept>

namespace sketch {

TopoDS_Shape make_document_opening_host_shape(
    const DocumentSnapshot& source,const std::string& opening_id,std::size_t* cumulative_native_work) {
    return make_document_opening_host_shape(source.entities(),opening_id,cumulative_native_work);
}

TopoDS_Shape make_document_opening_host_shape(
    const std::map<std::string, Entity, std::less<>>& actual,const std::string& opening_id,
    std::size_t* cumulative_native_work) {
    constexpr std::size_t entity_limit=65536,phase_limit=2000000,cut_limit=512;
    if (actual.size()>entity_limit)
        throw std::invalid_argument("Opening host source exceeds the entity budget.");
    std::size_t phase_work{};
    for (const auto& [id,entity]:actual) {
        (void)id;
        if (entity.type!="model_phases") continue;
        const auto& model=entity.properties.at("model");
        const auto& members=model.at("entity_ids");
        const auto& alternatives=model.at("alternatives");
        if (!members.is_array() || members.size()>entity_limit || !alternatives.is_array() ||
            alternatives.size()>4096 || members.size()>(phase_limit-phase_work)/(alternatives.size()+1))
            throw std::invalid_argument("Opening host source exceeds the phase work budget.");
        phase_work+=members.size()*(alternatives.size()+1);
    }
    const auto opening=actual.find(opening_id);
    if (opening==actual.end() || opening->second.id!=opening_id || opening->second.type!="opening")
        throw std::invalid_argument("Opening host must resolve to an actual semantic opening.");
    validate_hosted_opening_profile_entity(opening->second);
    std::string wall_id,error;
    if (!read_document_wall_id(opening->second,wall_id,error)) throw std::invalid_argument(error);
    const auto host=actual.find(wall_id);
    if (host==actual.end() || host->second.id!=wall_id || host->second.type!="wall")
        throw std::invalid_argument("Opening host wall is missing or invalid.");
    const auto scope=constraint_phase_scope(actual);
    if (scope.inactive_owner_ids.contains(opening_id) || scope.inactive_owner_ids.contains(wall_id))
        throw std::invalid_argument("Opening host and wall must be active in the saved design.");
    std::vector<const Entity*> siblings;
    for (const auto& [id,entity]:actual) {
        if (entity.type!="opening" || scope.inactive_owner_ids.contains(id)) continue;
        const auto host_id=entity.properties.find("wall_id");
        if (host_id==entity.properties.end() || !host_id->is_string() || *host_id!=wall_id) continue;
        if (entity.id!=id || siblings.size()>=cut_limit)
            throw std::invalid_argument("Opening host has invalid or excessive sibling cuts.");
        validate_hosted_opening_profile_entity(entity);
        siblings.push_back(&entity);
    }
    Wall wall;
    if (!read_document_wall(resolve_vertical_placement(actual,host->second),siblings,wall,error))
        throw std::invalid_argument(error);
    validate_wall_semantics(wall);
    const auto cut=std::find_if(wall.openings.begin(),wall.openings.end(),
        [&](const auto& value){return value.id==opening_id;});
    if (cut==wall.openings.end())
        throw std::invalid_argument("Opening is absent from its actual host wall.");
    const auto& properties=opening->second.properties;
    std::optional<OpeningAssembly> assembly;
    if (properties.contains("opening_assembly"))
        assembly=parse_opening_assembly(properties.at("opening_assembly"));
    else if (properties.contains("opening_kind")) {
        const auto kind=parse_opening_assembly_kind(properties.at("opening_kind").get<std::string>());
        if (kind) assembly=default_opening_assembly(*kind);
    }
    if (!assembly)
        throw std::invalid_argument("A bare opening has no manufactured body for a legacy host-copy component.");
    std::optional<DoorOperation> operation;
    if (properties.contains("door_operation")) operation=decode_door_operation(properties.at("door_operation"));
    // Layer cuts multiply; moving manufactured families may reconstruct their
    // host repeatedly. Reserve the complete conservative factory work before
    // entering any native builder, including repeated legacy-copy requests.
    constexpr std::size_t native_limit=262144,wall_factory_count=33;
    const auto layers=std::max<std::size_t>(1,wall.layers.size());
    const auto cuts=wall.openings.size()+1;
    if (layers>native_limit/cuts/wall_factory_count)
        throw std::invalid_argument("Opening host exceeds the layered cut geometry budget.");
    const auto work=layers*cuts*wall_factory_count;
    if (cumulative_native_work) {
        if (*cumulative_native_work>native_limit || work>native_limit-*cumulative_native_work)
            throw std::invalid_argument("Opening host copies exceed the cumulative native geometry budget.");
        *cumulative_native_work+=work;
    }
    (void)make_wall(wall);
    return make_opening_assembly(wall,*cut,*assembly,operation);
}

} // namespace sketch
