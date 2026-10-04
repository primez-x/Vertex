#include "sketch/document.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/room_relationship_geometry.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/reference_grid.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/georeferencing_entity_codec.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_split.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/survey_source_version.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <random>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

namespace sketch {
namespace {

constexpr std::size_t kMaximumIdBytes = 128;
constexpr std::size_t kMaximumTypeBytes = 64;
constexpr std::size_t kMaximumEntityJsonBytes = 1024 * 1024;
constexpr std::size_t kMaximumAssetBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::size_t kMaximumJsonValues = 100'000;

[[noreturn]] void document_error(DocumentErrorCode code, const std::string& message) {
    throw DocumentError(code, message);
}

bool is_valid_identifier(std::string_view value) {
    if (value.empty() || value.size() > kMaximumIdBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.' || character == ':';
    });
}

bool is_valid_utf8_without_nul(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first == 0) {
            return false;
        }
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        unsigned char second_minimum = 0x80U;
        unsigned char second_maximum = 0xbfU;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2;
            if (first == 0xe0U) {
                second_minimum = 0xa0U;
            } else if (first == 0xedU) {
                second_maximum = 0x9fU;
            }
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3;
            if (first == 0xf0U) {
                second_minimum = 0x90U;
            } else if (first == 0xf4U) {
                second_maximum = 0x8fU;
            }
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if (second < second_minimum || second > second_maximum) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if (continuation < 0x80U || continuation > 0xbfU) {
                return false;
            }
        }
        index += continuation_count + 1;
    }
    return true;
}

void validate_json_value(const nlohmann::json& value, std::size_t depth, std::size_t& count,
                         DocumentErrorCode error_code, std::string_view context) {
    if (++count > kMaximumJsonValues || depth > kMaximumJsonDepth) {
        document_error(error_code, std::string(context) + " exceeds JSON complexity limits");
    }
    if (value.is_discarded() || value.is_binary()) {
        document_error(error_code, std::string(context) + " contains a non-portable JSON value");
    }
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        document_error(error_code, std::string(context) + " contains a non-finite number");
    }
    if (value.is_array()) {
        for (const auto& child : value) {
            validate_json_value(child, depth + 1, count, error_code, context);
        }
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key.size() > kMaximumIdBytes) {
                document_error(error_code, std::string(context) + " contains an oversized key");
            }
            validate_json_value(child, depth + 1, count, error_code, context);
        }
    }
}

void validate_json_object(const nlohmann::json& value, DocumentErrorCode error_code,
                          std::string_view context) {
    if (!value.is_object()) {
        document_error(error_code, std::string(context) + " must be a JSON object");
    }
    std::size_t count = 0;
    validate_json_value(value, 0, count, error_code, context);
    try {
        if (value.dump().size() > kMaximumEntityJsonBytes) {
            document_error(error_code, std::string(context) + " exceeds the encoded size limit");
        }
    } catch (const nlohmann::json::exception& error) {
        document_error(error_code, std::string(context) + " cannot be encoded: " + error.what());
    }
}

struct StairConnectionReference {
    std::string graph_entity_id;
    std::string link_id;
    std::string lower_level_id;
    std::string upper_level_id;
};

std::optional<StairConnectionReference> stair_connection_reference(const Entity& entity) {
    const auto found = entity.properties.find("level_connection");
    if (found == entity.properties.end()) {
        return std::nullopt;
    }
    if (entity.type != "stair") {
        document_error(DocumentErrorCode::invalid_entity,
                       "level_connection requires a stair entity");
    }
    const auto& value = found.value();
    if (!value.is_object() || value.size() != 5 ||
        !value.contains("version") || !value.contains("graph_id") ||
        !value.contains("link_id") || !value.contains("lower_level_id") ||
        !value.contains("upper_level_id")) {
        document_error(DocumentErrorCode::invalid_entity,
                       "stair level_connection must contain exactly version, graph_id, link_id, lower_level_id, and upper_level_id");
    }
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || version != 1) {
        document_error(DocumentErrorCode::invalid_entity,
                       "stair level_connection version must be 1");
    }
    const auto read_id = [&](std::string_view key, std::size_t maximum) {
        const auto& candidate = value.at(std::string(key));
        if (!candidate.is_string()) {
            document_error(DocumentErrorCode::invalid_entity,
                           "stair level_connection " + std::string(key) + " must be a string");
        }
        const auto result = candidate.get<std::string>();
        if (result.empty() || result.size() > maximum ||
            !is_valid_utf8_without_nul(result)) {
            document_error(DocumentErrorCode::invalid_entity,
                           "stair level_connection " + std::string(key) + " is invalid");
        }
        return result;
    };
    const auto graph_entity_id = read_id("graph_id", kMaximumIdBytes);
    if (!is_valid_identifier(graph_entity_id)) {
        document_error(DocumentErrorCode::invalid_entity,
                       "stair level_connection graph_id is not a valid entity ID");
    }
    StairConnectionReference result{
        graph_entity_id,
        read_id("link_id", 256),
        read_id("lower_level_id", 256),
        read_id("upper_level_id", 256),
    };
    if (result.lower_level_id == result.upper_level_id) {
        document_error(DocumentErrorCode::invalid_entity,
                       "stair level_connection lower and upper levels must differ");
    }
    return result;
}

void validate_room_volume(const Entity& entity) {
    const auto& properties = entity.properties;
    const auto field = [&](const char* canonical, const char* legacy) -> const nlohmann::json* {
        auto found = properties.find(canonical);
        if (found == properties.end()) found = properties.find(legacy);
        return found == properties.end() ? nullptr : &*found;
    };
    const auto* height = field("height_m", "height");
    const auto* elevation = field("elevation_m", "elevation");
    // Historical plan-only rooms deliberately lack one or both volume fields.
    if (height == nullptr || elevation == nullptr) return;
    try {
        const auto number = [](const nlohmann::json& value) {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("coordinates and dimensions must be finite numbers");
            return value.get<double>();
        };
        if (number(*height) <= default_geometry_tolerance_metres)
            throw std::invalid_argument("height must be positive");
        (void)number(*elevation);
        const auto boundary = [&](const nlohmann::json& value) {
            if (!value.is_array() || value.empty())
                throw std::invalid_argument("boundary must be a nonempty segment array");
            const auto point = [&](const nlohmann::json& coordinates) -> Vec2 {
                if (!coordinates.is_array() || coordinates.size() != 2)
                    throw std::invalid_argument("boundary point must be [x, y]");
                return {number(coordinates[0]), number(coordinates[1])};
            };
            Boundary result;
            result.reserve(value.size());
            for (const auto& edge : value)
                result.push_back({point(edge.at("start")), point(edge.at("end")),
                                  number(edge.at("sweep_radians"))});
            return result;
        };
        const auto* outer_value = field("boundary", "segments");
        if (outer_value == nullptr) throw std::invalid_argument("boundary is required");
        const auto outer = boundary(*outer_value);
        std::vector<Boundary> holes;
        if (const auto found = properties.find("holes"); found != properties.end()) {
            if (!found->is_array()) throw std::invalid_argument("holes must be an array");
            holes.reserve(found->size());
            for (const auto& value : *found) holes.push_back(boundary(value));
        }
        if (const auto error = validate_boundary_holes(outer, holes))
            throw std::invalid_argument(*error);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity,
                       "invalid room volume " + entity.id + ": " + error.what());
    }
}

void validate_entity(const Entity& entity) {
    if (!is_valid_identifier(entity.id)) {
        document_error(DocumentErrorCode::invalid_entity, "entity id is empty or invalid");
    }
    if (entity.type.empty() || entity.type.size() > kMaximumTypeBytes ||
        !std::all_of(entity.type.begin(), entity.type.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '_';
        })) {
        document_error(DocumentErrorCode::invalid_entity, "entity type is empty or invalid");
    }
    validate_json_object(entity.properties, DocumentErrorCode::invalid_entity,
                         "entity properties");
    validate_json_object(entity.extensions, DocumentErrorCode::invalid_entity,
                         "entity extensions");
    static constexpr std::array reserved{"id", "type", "required", "properties"};
    for (const auto* key : reserved) {
        if (entity.extensions.contains(key)) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("entity extension uses reserved field: ") + key);
        }
    }
    if (entity.type == "measurement_linework") {
        try {
            if (!entity.required)
                throw std::invalid_argument("measurement linework must be required geometry");
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            if (decoded.supported() && decoded.model->stroke_id != entity.id)
                throw std::invalid_argument("measurement linework stroke ID must match its entity ID");
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           "invalid measurement linework " + entity.id + ": " + error.what());
        }
    }
    if (entity.type == kSheetViewEntityType) {
        try {
            validate_sheet_view_entity(entity);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid sheet/view entity: ") + error.what());
        }
    }
    if (entity.type == kAnnotationEntityType) {
        try {
            validate_annotation_entity(entity);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid annotation entity: ") + error.what());
        }
    }
    if (entity.type == kGeoreferencingEntityType) {
        try {
            validate_georeferencing_entity(entity);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid georeferencing entity: ") + error.what());
        }
    }
    if (entity.type == "room") validate_room_volume(entity);
    (void)stair_connection_reference(entity);
    if (entity.properties.contains("vertical_level_binding")) {
        if (entity.type != "floor") {
            document_error(DocumentErrorCode::invalid_entity,
                           "vertical level binding requires a floor entity");
        }
        try {
            (void)VerticalLevelBinding::from_json(entity.properties.at("vertical_level_binding"));
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid vertical level binding: ") + error.what());
        }
    }
    if (entity.type == "slab" && entity.properties.contains("element_kind")) {
        const auto& value = entity.properties.at("element_kind");
        const bool valid = value.is_string() &&
            (value == "slab" || value == "floor" || value == "ceiling" || value == "foundation");
        if (!valid) {
            document_error(DocumentErrorCode::invalid_entity,
                           "slab element_kind must be slab, floor, ceiling, or foundation");
        }
    }
    if (entity.type == "wall" && entity.properties.contains("layers")) {
        try {
            const auto thickness = entity.properties.find("thickness_m");
            const auto legacy_thickness = entity.properties.find("thickness");
            const auto* thickness_value = thickness != entity.properties.end() ? &thickness.value() :
                                          (legacy_thickness != entity.properties.end() ? &legacy_thickness.value() : nullptr);
            if (thickness_value == nullptr || !thickness_value->is_number()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "wall layers require a numeric thickness_m");
            }
            (void)parse_wall_layers(entity.properties.at("layers"), thickness_value->get<double>());
        } catch (const DocumentError&) {
            throw;
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid wall layers: ") + error.what());
        }
    }
    if (entity.type == "slab" && entity.properties.contains("layers")) {
        try {
            const auto thickness = entity.properties.find("thickness_m");
            const auto legacy_thickness = entity.properties.find("thickness");
            const auto* thickness_value = thickness != entity.properties.end() ? &thickness.value() :
                                          (legacy_thickness != entity.properties.end() ? &legacy_thickness.value() : nullptr);
            if (thickness_value == nullptr || !thickness_value->is_number()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "slab layers require a numeric thickness_m");
            }
            (void)parse_slab_layers(entity.properties.at("layers"), thickness_value->get<double>());
        } catch (const DocumentError&) {
            throw;
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid slab layers: ") + error.what());
        }
    }
    if (entity.type == "wall" && entity.properties.contains("slope_rise_m")) {
        const auto& slope = entity.properties.at("slope_rise_m");
        if (!slope.is_number() ||
            (slope.is_number_float() && !std::isfinite(slope.get<double>()))) {
            document_error(DocumentErrorCode::invalid_entity,
                           "wall slope_rise_m must be a finite number");
        }
    }
    if (entity.properties.contains("opening_assembly")) {
        if (entity.type != "opening") {
            document_error(DocumentErrorCode::invalid_entity,
                           "Opening assembly requires an opening entity");
        }
        try {
            const auto assembly = parse_opening_assembly(
                entity.properties.at("opening_assembly"));
            const auto kind = entity.properties.find("opening_kind");
            if (kind == entity.properties.end() || !kind->is_string() ||
                kind->get<std::string>() != opening_assembly_kind_name(assembly.kind)) {
                document_error(DocumentErrorCode::invalid_entity,
                               "Opening assembly kind must match opening_kind");
            }
        } catch (const DocumentError&) {
            throw;
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("Invalid opening assembly: ") + error.what());
        }
    }
    if (entity.type == "wall_join") {
        try {
            (void)parse_wall_join(entity.properties, entity.id);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid wall join entity: ") + error.what());
        }
    }
    if (entity.type == "roof_join") {
        try {
            (void)parse_roof_join(entity.properties, entity.id);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid roof join entity: ") + error.what());
        }
    }
    const auto validate_embedded_model = [&](auto decoder, std::string_view name) {
        if (!entity.properties.contains("model")) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string(name) + " entity requires a model object");
        }
        try {
            decoder(entity.properties.at("model"));
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid ") + std::string(name) + " entity: " + error.what());
        }
    };
    if (entity.type == "assembly_model") {
        validate_embedded_model([](const nlohmann::json& model) {
            (void)AssemblyModel::from_json(model);
        }, "assembly model");
    } else if (entity.type == "model_phases") {
        validate_embedded_model([](const nlohmann::json& model) {
            (void)ModelPhases::from_json(model);
        }, "model phases");
    } else if (entity.type == "room_relationships") {
        validate_embedded_model([](const nlohmann::json& model) {
            if (room_relationship_model_version(model) <= 2)
                (void)RoomRelationshipSnapshot::from_json(model);
        }, "room relationships");
    } else if (entity.type == "vertical_levels") {
        validate_embedded_model([](const nlohmann::json& model) {
            (void)VerticalLevelGraph::from_json(model);
        }, "vertical levels");
    } else if (entity.type == "reference_grid") {
        validate_embedded_model([](const nlohmann::json& model) {
            (void)ReferenceGridModel::from_json(model);
        }, "reference grid");
    } else if (entity.type == "terrain_surface") {
        validate_embedded_model([](const nlohmann::json& model) {
            (void)TerrainSurface::from_json(model);
        }, "terrain surface");
    }
}

void validate_asset(const Asset& asset) {
    if (!is_valid_identifier(asset.id)) {
        document_error(DocumentErrorCode::invalid_asset, "asset id is empty or invalid");
    }
    if (asset.media_type.empty() || asset.media_type.size() > 256 ||
        asset.media_type.find_first_of("\r\n") != std::string::npos ||
        !is_valid_utf8_without_nul(asset.media_type)) {
        document_error(DocumentErrorCode::invalid_asset, "asset media type is invalid");
    }
    if (asset.bytes.size() > kMaximumAssetBytes) {
        document_error(DocumentErrorCode::invalid_asset, "asset exceeds the byte limit");
    }
    validate_json_object(asset.metadata, DocumentErrorCode::invalid_asset, "asset metadata");
    if (asset.sha256.size() != 64 ||
        !std::all_of(asset.sha256.begin(), asset.sha256.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }) ||
        sha256_hex(asset.bytes) != asset.sha256) {
        document_error(DocumentErrorCode::invalid_asset, "asset SHA-256 does not match its bytes");
    }
}

struct EntityReference {
    enum class Target { entity, asset };

    std::string id;
    std::optional<std::string_view> expected_type;
    Target target = Target::entity;
};

std::optional<std::optional<std::string_view>> reference_type_for_key(std::string_view key) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 18> typed{{
        {"property_id", "property"},
        {"building_id", "building"},
        {"floor_id", "floor"},
        {"layer_id", "layer"},
        {"boundary_id", "boundary"},
        {"wall_id", "wall"},
        {"opening_id", "opening"},
        {"room_id", "room"},
        {"slab_id", "slab"},
        {"roof_id", "roof"},
        {"stair_id", "stair"},
        {"sheet_id", "sheet"},
        {"view_id", "view"},
        {"constraint_id", "constraint"},
        {"label_id", "label"},
        {"column_id", "column"},
        {"beam_id", "beam"},
        {"railing_id", "railing"},
    }};
    for (const auto& [candidate, type] : typed) {
        if (key == candidate) {
            return type;
        }
        if (key.size() == candidate.size() + 1 && key.back() == 's' &&
            key.substr(0, candidate.size()) == candidate) {
            return type;
        }
    }
    if (key == "parent_id" || key == "host_id" || key == "target_id" ||
        key == "entity_id" || key == "source_entity_id" || key == "parent_ids" ||
        key == "host_ids" || key == "target_ids" || key == "entity_ids" ||
        key == "source_entity_ids") {
        return std::optional<std::string_view>{};
    }
    return std::nullopt;
}

void collect_references(const Entity& entity, std::vector<EntityReference>& references) {
    if (!is_known_entity_type(entity.type)) {
        return;
    }
    if (const auto connection = stair_connection_reference(entity)) {
        references.push_back({connection->graph_entity_id, "vertical_levels",
                              EntityReference::Target::entity});
    }
    if (entity.type == "wall" && entity.properties.contains("layers")) {
        const auto thickness = entity.properties.find("thickness_m");
        const auto legacy_thickness = entity.properties.find("thickness");
        const auto* thickness_value = thickness != entity.properties.end() ? &thickness.value() :
                                      (legacy_thickness != entity.properties.end() ? &legacy_thickness.value() : nullptr);
        if (thickness_value == nullptr || !thickness_value->is_number()) {
            document_error(DocumentErrorCode::invalid_entity,
                           "wall layers require a numeric thickness_m");
        }
        try {
            for (const auto& layer : parse_wall_layers(entity.properties.at("layers"),
                                                        thickness_value->get<double>())) {
                if (layer.material.has_value()) {
                    references.push_back({layer.material->catalog_id, "assembly_model",
                                          EntityReference::Target::entity});
                }
            }
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid wall layer reference: ") + error.what());
        }
    }
    if (entity.type == "slab" && entity.properties.contains("layers")) {
        const auto thickness = entity.properties.find("thickness_m");
        const auto legacy_thickness = entity.properties.find("thickness");
        const auto* thickness_value = thickness != entity.properties.end() ? &thickness.value() :
                                      (legacy_thickness != entity.properties.end() ? &legacy_thickness.value() : nullptr);
        if (thickness_value == nullptr || !thickness_value->is_number()) {
            document_error(DocumentErrorCode::invalid_entity,
                           "slab layers require a numeric thickness_m");
        }
        try {
            for (const auto& layer : parse_slab_layers(entity.properties.at("layers"),
                                                       thickness_value->get<double>())) {
                if (layer.material.has_value()) {
                    references.push_back({layer.material->catalog_id, "assembly_model",
                                          EntityReference::Target::entity});
                }
            }
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid slab layer reference: ") + error.what());
        }
    }
    if (entity.type == "assembly_model" && entity.properties.contains("model")) {
        try {
            const auto model = AssemblyModel::from_json(entity.properties.at("model"));
            // Placement hosts are semantic references nested inside the
            // versioned catalog payload, so surface them through the same
            // dangling-reference guard used by ordinary entity properties.
            for (const auto& instance : model.instances()) {
                if (instance.placement) {
                    references.push_back({instance.placement->host_entity_id, std::nullopt,
                                          EntityReference::Target::entity});
                }
            }
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid assembly placement data: ") + error.what());
        }
    }
    for (const auto& [key, value] : entity.properties.items()) {
        if (key == "vertical_level_binding") {
            try {
                const auto binding = VerticalLevelBinding::from_json(value);
                references.push_back({binding.graph_entity_id, "vertical_levels",
                                      EntityReference::Target::entity});
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               std::string("invalid vertical level binding: ") + error.what());
            }
            continue;
        }
        if (key == "refs" || key == "references") {
            if (!value.is_array()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "reference collection must be a JSON array");
            }
            for (const auto& reference : value) {
                if (!reference.is_string()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "reference collection contains a non-string id");
                }
                references.push_back(
                    {reference.get<std::string>(), std::nullopt, EntityReference::Target::entity});
            }
            continue;
        }
        if (key == "asset_id" || key == "asset_ids" || key == "render_asset_id" ||
            key == "render_asset_ids") {
            const bool collection = key == "asset_ids" || key == "render_asset_ids";
            if (collection) {
                if (!value.is_array()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "asset_ids must be a JSON array");
                }
                for (const auto& reference : value) {
                    if (!reference.is_string()) {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "asset_ids contains a non-string id");
                    }
                    references.push_back({reference.get<std::string>(), std::nullopt,
                                          EntityReference::Target::asset});
                }
            } else {
                if (!value.is_string()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "asset_id must contain a string id");
                }
                references.push_back({value.get<std::string>(), std::nullopt,
                                      EntityReference::Target::asset});
            }
            continue;
        }
        const auto expected_type = reference_type_for_key(key);
        if (!expected_type.has_value()) {
            continue;
        }
        const bool collection = key.size() >= 4 && key.substr(key.size() - 4) == "_ids";
        if (collection) {
            if (!value.is_array()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "canonical reference collection must be a JSON array");
            }
            for (const auto& reference : value) {
                if (!reference.is_string()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "canonical reference collection contains a non-string id");
                }
                references.push_back({reference.get<std::string>(), *expected_type,
                                      EntityReference::Target::entity});
            }
        } else {
            if (!value.is_string()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "canonical reference property must contain a string id");
            }
            references.push_back(
                {value.get<std::string>(), *expected_type, EntityReference::Target::entity});
        }
    }
}

// Loose strokes retain measurement geometry without wall or area semantics.
// Organizational references stay known when a future model is opaque.
std::optional<std::string> validate_measurement_linework_integrity(
    const std::map<std::string, Entity, std::less<>>& entities) {
    if (std::none_of(entities.begin(), entities.end(), [](const auto& item) {
            return item.second.type == "measurement_linework";
        })) return std::nullopt;
    std::optional<std::string> unsupported;
    std::set<std::string, std::less<>> occupied;
    for (const auto& [id, entity] : entities) {
        occupied.insert(id);
        if (can_recognize_boundary_entity_type(entity.type) &&
            inspect_boundary_entity_version(entity).format == BoundaryEntityFormat::identified_v1) {
            for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
                occupied.insert(edge.segment_id);
                occupied.insert(edge.start_vertex_id);
                occupied.insert(edge.end_vertex_id);
            }
        }
    }
    const auto organization = organize_project(entities);
    const auto reference = [](const Entity& entity, const char* key) -> std::string {
        const auto found = entity.properties.find(key);
        if (found == entity.properties.end() || !found->is_string() ||
            found->get_ref<const std::string&>().empty())
            throw std::invalid_argument("measurement linework requires a complete drawing context: " +
                                        std::string(key));
        return found->get<std::string>();
    };
    for (const auto& [id, entity] : entities) {
        if (entity.type != "measurement_linework") continue;
        const auto property = reference(entity, "property_id");
        const auto building = reference(entity, "building_id");
        const auto floor = reference(entity, "floor_id");
        const auto layer = reference(entity, "layer_id");
        const auto resolved = organization.drawing_context(id);
        if (!resolved || *resolved != DrawingContext{property, building, floor, layer})
            throw std::invalid_argument("measurement linework " + id +
                                        " has inconsistent drawing context references");
        const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
        if (!decoded.supported()) {
            if (!unsupported) unsupported = "measurement linework " + id + ": " + decoded.diagnostic;
            continue;
        }
        std::set<std::string, std::less<>> vertices;
        for (const auto& edge : decoded.model->edges) {
            if (!occupied.insert(edge.segment_id).second)
                throw std::invalid_argument("measurement linework segment identity collides: " + edge.segment_id);
            vertices.insert(edge.start_vertex_id);
            vertices.insert(edge.end_vertex_id);
        }
        for (const auto& vertex : vertices)
            if (!occupied.insert(vertex).second)
                throw std::invalid_argument("measurement linework vertex identity collides: " + vertex);
    }
    return unsupported;
}

std::optional<std::string> validate_state(const std::map<std::string, Entity, std::less<>>& entities,
                    const std::map<std::string, Asset, std::less<>>& assets) {
    std::optional<std::string> unsupported_relationship;
    std::map<std::string, RoomReference, std::less<>> room_references;
    std::vector<RoomRelation> room_relations;
    std::set<std::tuple<std::string, std::string, RoomRelationKind>> room_relation_keys;
    for (const auto& [id, entity] : entities) {
        validate_entity(entity);
        if (entity.type=="wall") {
            try {
                const auto input=entity.properties.find("original_drawing_input");
                const bool known_typed=input!=entity.properties.end() && input->is_object() && input->contains("version") &&
                    input->at("version").is_number_integer() && input->at("version")==2;
                if (input!=entity.properties.end() && input->is_object() && input->contains("chord_input") && !known_typed) {
                    const auto discriminator=input->find("version");
                    const bool future=discriminator!=input->end() && discriminator->is_number_integer() &&
                        (discriminator->is_number_unsigned() ? discriminator->get<std::uint64_t>()>2 : discriminator->get<std::int64_t>()>2);
                    if (!future) throw std::invalid_argument("Typed wall chord input requires its receipt version discriminator");
                }
                if (known_typed) {
                    // A future context cannot conceal malformed known receipt inputs.
                    const auto receipt=decode_construction_receipt(*input);
                    (void)replay_construction_receipt(receipt,{receipt.start});
                    const auto saved=entity.properties.find("original_drawing_input_context");
                    if (saved==entity.properties.end() || !saved->is_object() || !saved->contains("version") ||
                        !saved->at("version").is_number_integer())
                        throw std::invalid_argument("Typed wall chord input requires a saved replay context");
                    // Future context dialects remain historical opaque metadata.
                    if (saved->at("version")==1) {
                        if (saved->size()!=5 || !saved->contains("expected_start") || !saved->contains("previous_segment") ||
                            !saved->contains("closure_anchor") || !saved->contains("tolerance_metres") ||
                            !saved->at("previous_segment").is_null() || !saved->at("closure_anchor").is_null())
                            throw std::invalid_argument("Typed wall chord replay context has unsupported fields");
                        const auto& start=saved->at("expected_start");
                        if (!start.is_array() || start.size()!=2 || !start[0].is_number() || !start[1].is_number() ||
                            !saved->at("tolerance_metres").is_number())
                            throw std::invalid_argument("Typed wall chord replay context is malformed");
                        const ConstructionReplayContext context{{start[0].get<double>(),start[1].get<double>()},
                            std::nullopt,std::nullopt,saved->at("tolerance_metres").get<double>()};
                        (void)replay_construction_receipt(receipt,context);
                    } else if (saved->at("version").is_number_unsigned() ?
                        saved->at("version").get<std::uint64_t>()==0 : saved->at("version").get<std::int64_t>()<=0)
                        throw std::invalid_argument("Wall chord replay context version must be positive");
                }
                if (entity.extensions.contains("curve_input_derivation")) validate_wall_curve_input(entity);
                validate_wall_length_input(entity);
                if(entity.extensions.contains("wall_split_archive")) {
                    const auto& archive=entity.extensions.at("wall_split_archive");
                    if(!archive.is_object() || !archive.contains("version") || !archive.at("version").is_number_integer() ||
                        archive.at("version").get<std::int64_t>()<=0)
                        throw std::invalid_argument("Wall split archive requires a positive version: "+id);
                    if(archive.at("version")==1)validate_wall_split_archive(entity);
                }
            }
            catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
        }
        if (id != entity.id) {
            document_error(DocumentErrorCode::invalid_entity,
                           "entity map key does not match its stable id");
        }
    }
    for (const auto& [id, asset] : assets) {
        validate_asset(asset);
        if (id != asset.id) {
            document_error(DocumentErrorCode::invalid_asset,
                           "asset map key does not match its stable id");
        }
    }
    for (const auto& [id, entity] : entities) {
        std::vector<EntityReference> references;
        collect_references(entity, references);
        for (const auto& reference : references) {
            if (reference.target == EntityReference::Target::asset) {
                if (!assets.contains(reference.id)) {
                    document_error(DocumentErrorCode::dangling_reference,
                                   "entity " + id + " references missing asset " + reference.id);
                }
                continue;
            }
            const auto target = entities.find(reference.id);
            if (target == entities.end()) {
                document_error(DocumentErrorCode::dangling_reference,
                               "entity " + id + " references missing entity " + reference.id);
            }
            if (reference.expected_type.has_value() &&
                target->second.type != *reference.expected_type) {
                document_error(DocumentErrorCode::invalid_entity,
                               "entity " + id + " reference " + reference.id +
                                   " has type " + target->second.type + ", expected " +
                                   std::string(*reference.expected_type));
            }
        }
        if (entity.type == kSheetViewEntityType) {
            try {
                const auto model = decode_sheet_view_entity(entity);
                for (const auto& view : model.views()) {
                    for (const auto& object_id : view.object_ids) {
                        if (!entities.contains(object_id)) {
                            document_error(
                                DocumentErrorCode::dangling_reference,
                                "coordinated view " + view.id +
                                    " references missing object " + object_id);
                        }
                    }
                    if (view.presentation.appearance) {
                        for (const auto& appearance : view.presentation.appearance->objects) {
                            if (!entities.contains(appearance.object_id)) {
                                document_error(DocumentErrorCode::dangling_reference,
                                    "coordinated view " + view.id +
                                    " appearance references missing object " + appearance.object_id);
                            }
                        }
                    }
                    for (const auto& overlay : view.overlays) {
                        if (!overlay.dimension_binding) continue;
                        const auto& target_id = overlay.dimension_binding->object_id;
                        const auto target = entities.find(target_id);
                        if (target == entities.end()) {
                            document_error(DocumentErrorCode::dangling_reference,
                                "section dimension " + overlay.id + " references missing object " + target_id);
                        }
                        static constexpr std::array<std::string_view, 11> dimension_types{
                            "wall", "opening", "room", "slab", "roof", "stair", "railing",
                            "column", "beam", "wall_join", "roof_join"};
                        if (std::find(dimension_types.begin(), dimension_types.end(), target->second.type) == dimension_types.end()) {
                            document_error(DocumentErrorCode::invalid_entity,
                                "section dimension " + overlay.id + " references unsupported object " + target_id);
                        }
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid coordinated-view object references in " + id +
                                   ": " + error.what());
            }
        }
        if (entity.type == "assembly_model" && entity.properties.contains("model")) {
            const auto model = AssemblyModel::from_json(entity.properties.at("model"));
            for (const auto& instance : model.instances()) {
                if (!instance.placement) continue;
                const auto host = entities.find(instance.placement->host_entity_id);
                if (host == entities.end()) continue; // reported by the reference pass above
                static constexpr std::array<std::string_view, 12> host_types{
                    "boundary", "measurement_boundary", "room_boundary", "wall", "opening",
                    "slab", "roof", "stair", "railing", "column", "beam", "terrain_surface"};
                if (std::find(host_types.begin(), host_types.end(), host->second.type) == host_types.end()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "assembly placement host " + instance.placement->host_entity_id +
                                       " is not a geometry-bearing architectural entity");
                }
            }
        }
        if (entity.type == "floor" && entity.properties.contains("vertical_level_binding")) {
            try {
                const auto binding = VerticalLevelBinding::from_json(
                    entity.properties.at("vertical_level_binding"));
                const auto graph = entities.find(binding.graph_entity_id);
                if (graph == entities.end()) {
                    // The canonical reference pass above reports the missing graph.
                    continue;
                }
                const auto model = VerticalLevelGraph::from_json(graph->second.properties.at("model"));
                const auto level = std::find_if(model.levels().begin(), model.levels().end(),
                    [&](const auto& candidate) { return candidate.id == binding.level_id; });
                if (level == model.levels().end()) {
                    document_error(DocumentErrorCode::dangling_reference,
                                   "floor " + id + " vertical level binding references missing level " +
                                       binding.level_id);
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid floor vertical level binding " + id + ": " + error.what());
            }
        }
        if (entity.type == "wall_join") {
            try {
                const auto join = parse_wall_join(entity.properties, entity.id);
                for (const auto& wall_id : join.wall_ids) {
                    const auto wall = entities.find(wall_id);
                    if (wall == entities.end()) {
                        // The canonical reference pass above reports the
                        // missing wall; keep this semantic pass deterministic.
                        continue;
                    }
                    if (wall->second.type != "wall") {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "wall join " + id + " references " + wall_id +
                                       " with type " + wall->second.type + ", expected wall");
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid wall join references " + id + ": " + error.what());
            }
        }
        if (entity.type == "roof_join") {
            try {
                const auto join = parse_roof_join(entity.properties, entity.id);
                for (const auto& roof_id : join.roof_ids) {
                    const auto roof = entities.find(roof_id);
                    if (roof == entities.end()) {
                        // The canonical reference pass above reports the
                        // missing roof; keep this semantic pass deterministic.
                        continue;
                    }
                    if (roof->second.type != "roof") {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "roof join " + id + " references " + roof_id +
                                       " with type " + roof->second.type + ", expected roof");
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid roof join references " + id + ": " + error.what());
            }
        }
        if (const auto connection = stair_connection_reference(entity)) {
            const auto graph = entities.find(connection->graph_entity_id);
            if (graph == entities.end()) {
                // The canonical reference pass above reports the missing graph.
                continue;
            }
            if (graph->second.type != "vertical_levels") {
                document_error(DocumentErrorCode::invalid_entity,
                               "stair " + id + " level_connection graph is not a vertical_levels entity");
            }
            try {
                const auto model = VerticalLevelGraph::from_json(
                    graph->second.properties.at("model"));
                const auto link = std::find_if(model.links().begin(), model.links().end(),
                    [&](const auto& candidate) { return candidate.id == connection->link_id; });
                if (link == model.links().end()) {
                    document_error(DocumentErrorCode::dangling_reference,
                                   "stair " + id + " level_connection references missing link " +
                                       connection->link_id);
                }
                if (link->lower_level_id != connection->lower_level_id ||
                    link->upper_level_id != connection->upper_level_id) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "stair " + id + " level_connection endpoint IDs do not match its graph link");
                }
                if (link->state == RelationshipState::disconnected) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "stair " + id + " level_connection cannot use a disconnected graph link");
                }
                const auto rise = entity.properties.find("total_rise_m");
                if (rise != entity.properties.end()) {
                    if (!rise->is_number() || !std::isfinite(rise->get<double>()) ||
                        rise->get<double>() <= 0.0) {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "stair " + id + " total_rise_m must be finite and positive");
                    }
                    const auto expected = model.floor_to_floor_height(connection->link_id);
                    if (std::abs(rise->get<double>() - expected) >
                        VerticalLevelGraph::height_tolerance_m) {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "stair " + id + " total_rise_m does not match its connected floor-to-floor height");
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid stair level_connection " + id + ": " + error.what());
            }
        }
    }
    {
        std::map<std::string, std::string, std::less<>> wall_join_owners;
        for (const auto& [id, entity] : entities) {
            if (entity.type != "wall_join") continue;
            const auto join = parse_wall_join(entity.properties, id);
            for (const auto& wall_id : join.wall_ids) {
                const auto [owner, inserted] = wall_join_owners.emplace(wall_id, id);
                if (!inserted) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "wall " + wall_id + " belongs to multiple wall joins: " +
                                   owner->second + " and " + id);
                }
            }
        }
    }
    {
        std::map<std::string, std::string, std::less<>> roof_join_owners;
        for (const auto& [id, entity] : entities) {
            if (entity.type != "roof_join") continue;
            const auto join = parse_roof_join(entity.properties, id);
            for (const auto& roof_id : join.roof_ids) {
                const auto [owner, inserted] = roof_join_owners.emplace(roof_id, id);
                if (!inserted) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "roof " + roof_id + " belongs to multiple roof joins: " +
                                   owner->second + " and " + id);
                }
            }
        }
    }
    std::map<std::string, AssemblyModel> material_catalogs;
    for (const auto& [id, entity] : entities) {
        if (entity.properties.contains("door_operation")) {
            try {
                if (entity.type != "opening")
                    document_error(DocumentErrorCode::invalid_entity, "Door operation requires an opening");
                (void)decode_door_operation(entity.properties.at("door_operation"));
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, std::string("Invalid door operation: ") + error.what());
            }
        }
        if (entity.properties.contains("material_assignment")) {
            try {
                static constexpr std::array<std::string_view, 10> roles{
                    "wall", "opening", "room", "room_boundary", "slab", "roof", "stair", "railing", "column", "beam"};
                if (std::find(roles.begin(), roles.end(), entity.type) == roles.end())
                    document_error(DocumentErrorCode::invalid_entity, "Material assignment requires an architectural object");
                const auto& assignment = entity.properties.at("material_assignment");
                if (!assignment.is_object() || !assignment.at("version").is_number_integer() ||
                    assignment.at("version") != 1 || !assignment.at("catalog_id").is_string() ||
                    !assignment.at("material_id").is_string())
                    document_error(DocumentErrorCode::invalid_entity, "Invalid material assignment");
                const auto catalog_id = assignment.at("catalog_id").get<std::string>();
                const auto material_id = assignment.at("material_id").get<std::string>();
                const auto catalog = entities.find(catalog_id);
                if (catalog == entities.end())
                    document_error(DocumentErrorCode::dangling_reference, "Material assignment references a missing catalog");
                if (catalog->second.type != "assembly_model")
                    document_error(DocumentErrorCode::invalid_entity, "Material assignment target is not an assembly catalog");
                if (!material_catalogs.contains(catalog_id))
                    material_catalogs.emplace(catalog_id, AssemblyModel::from_json(catalog->second.properties.at("model")));
                const auto& materials = material_catalogs.at(catalog_id).materials();
                if (std::none_of(materials.begin(), materials.end(), [&](const auto& material) { return material.id == material_id; }))
                    document_error(DocumentErrorCode::dangling_reference, "Material assignment references a missing material");
            } catch (const DocumentError&) { throw; }
            catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, std::string("Invalid material assignment: ") + error.what());
            }
        }
        if (entity.type == "wall" && entity.properties.contains("layers")) {
            try {
                const auto thickness = entity.properties.find("thickness_m");
                const auto legacy_thickness = entity.properties.find("thickness");
                const auto* thickness_value = thickness != entity.properties.end() ? &thickness.value() :
                                              (legacy_thickness != entity.properties.end() ? &legacy_thickness.value() : nullptr);
                if (thickness_value == nullptr || !thickness_value->is_number()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "wall layers require a numeric thickness_m");
                }
                const auto layers = parse_wall_layers(entity.properties.at("layers"),
                                                      thickness_value->get<double>());
                for (const auto& layer : layers) {
                    if (!layer.material.has_value()) continue;
                    const auto& assignment = *layer.material;
                    const auto catalog = entities.find(assignment.catalog_id);
                    if (catalog == entities.end()) {
                        document_error(DocumentErrorCode::dangling_reference,
                                       "Wall layer material references a missing catalog");
                    }
                    if (catalog->second.type != "assembly_model") {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "Wall layer material target is not an assembly catalog");
                    }
                    if (!material_catalogs.contains(assignment.catalog_id)) {
                        material_catalogs.emplace(assignment.catalog_id,
                                                  AssemblyModel::from_json(catalog->second.properties.at("model")));
                    }
                    const auto& materials = material_catalogs.at(assignment.catalog_id).materials();
                    if (std::none_of(materials.begin(), materials.end(), [&](const auto& material) {
                            return material.id == assignment.material_id;
                        })) {
                        document_error(DocumentErrorCode::dangling_reference,
                                       "Wall layer material references a missing material");
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "Invalid wall layer material: " + std::string(error.what()));
            }
        }
        if (entity.type == "slab" && entity.properties.contains("layers")) {
            try {
                const auto thickness = entity.properties.find("thickness_m");
                const auto legacy_thickness = entity.properties.find("thickness");
                const auto* thickness_value = thickness != entity.properties.end() ? &thickness.value() :
                                              (legacy_thickness != entity.properties.end() ? &legacy_thickness.value() : nullptr);
                if (thickness_value == nullptr || !thickness_value->is_number()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "slab layers require a numeric thickness_m");
                }
                const auto layers = parse_slab_layers(entity.properties.at("layers"),
                                                      thickness_value->get<double>());
                for (const auto& layer : layers) {
                    if (!layer.material.has_value()) continue;
                    const auto& assignment = *layer.material;
                    const auto catalog = entities.find(assignment.catalog_id);
                    if (catalog == entities.end()) {
                        document_error(DocumentErrorCode::dangling_reference,
                                       "Slab layer material references a missing catalog");
                    }
                    if (catalog->second.type != "assembly_model") {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "Slab layer material target is not an assembly catalog");
                    }
                    if (!material_catalogs.contains(assignment.catalog_id)) {
                        material_catalogs.emplace(assignment.catalog_id,
                                                  AssemblyModel::from_json(catalog->second.properties.at("model")));
                    }
                    const auto& materials = material_catalogs.at(assignment.catalog_id).materials();
                    if (std::none_of(materials.begin(), materials.end(), [&](const auto& material) {
                            return material.id == assignment.material_id;
                        })) {
                        document_error(DocumentErrorCode::dangling_reference,
                                       "Slab layer material references a missing material");
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "Invalid slab layer material: " + std::string(error.what()));
            }
        }
        if (entity.type == "model_phases") {
            try {
                const auto model = ModelPhases::from_json(entity.properties.at("model"));
                const auto encoded = model.to_json();
                for (const auto& member : encoded.at("entity_ids")) {
                    const auto member_id = member.get<std::string>();
                    if (!entities.contains(member_id)) {
                        document_error(DocumentErrorCode::dangling_reference,
                                       "model phases " + id + " references missing entity " + member_id);
                    }
                    static constexpr std::array<std::string_view, 18> model_roles{
                        "building", "floor", "wall", "opening", "room", "room_boundary",
                        "slab", "roof", "stair", "railing", "column", "beam", "assembly_model",
                        "boundary", "measurement_boundary", "wall_join", "roof_join", "measurement_linework"};
                    const auto& type = entities.at(member_id).type;
                    if (std::find(model_roles.begin(), model_roles.end(), type) == model_roles.end()) {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "model phases " + id + " reference " + member_id +
                                       " is not an architectural model entity");
                    }
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid model phases entity " + id + ": " + error.what());
            }
        } else if (entity.type == "room_relationships") {
            try {
                const auto& payload = entity.properties.at("model");
                const auto version = room_relationship_model_version(payload);
                if (version > 2) {
                    if (!unsupported_relationship)
                        unsupported_relationship = "Unsupported room relationship model " +
                            std::to_string(version) + ": " + id;
                    continue;
                }
                const auto model = RoomRelationshipSnapshot::from_json(payload);
                for (const auto& reference : model.references()) {
                    const auto [definition, inserted] = room_references.emplace(reference.id, reference);
                    if (!inserted && definition->second != reference) {
                        document_error(DocumentErrorCode::invalid_entity,
                            "Room relationship records disagree on logical reference " + reference.id);
                    }
                    const auto expected = reference.kind == RoomReferenceKind::room_boundary
                        ? "room_boundary" : reference.kind == RoomReferenceKind::appraisal_measurement_boundary
                        ? "measurement_boundary" : "wall";
                    const auto members = room_reference_wall_ids(reference);
                    Boundary wall_path;
                    for (const auto& member : members) {
                        const auto target = entities.find(member);
                        if (target == entities.end()) {
                            document_error(DocumentErrorCode::dangling_reference,
                                "room relationships " + id + " references missing entity " + member);
                        }
                        if (target->second.type != expected) {
                            document_error(DocumentErrorCode::invalid_entity,
                                "room relationships " + id + " reference " + member +
                                " has type " + target->second.type + ", expected " + expected);
                        }
                        if (reference.kind == RoomReferenceKind::architectural_wall) {
                            // Reader admission shares the established wall decoder,
                            // including supported legacy property names. Typed edits
                            // retain their stricter authoring checks separately.
                            std::vector<const Entity*> openings;
                            for (const auto& [opening_id, opening] : entities) {
                                (void)opening_id;
                                if (opening.type != "opening") continue;
                                std::string host_id;
                                std::string diagnostic;
                                if (read_document_wall_id(opening, host_id, diagnostic) && host_id == member)
                                    openings.push_back(&opening);
                            }
                            Wall wall;
                            std::string diagnostic;
                            if (!read_document_wall(target->second, openings, wall, diagnostic))
                                document_error(DocumentErrorCode::invalid_entity,
                                    "Invalid relationship wall " + member + ": " + diagnostic);
                            validate_wall_semantics(wall);
                            wall_path.push_back(wall.baseline);
                        }
                    }
                    if (reference.kind == RoomReferenceKind::architectural_wall)
                        validate_room_relationship_wall_path(wall_path);
                }
                for (const auto& relation : model.relations()) {
                    if (room_relation_keys.emplace(relation.source_id, relation.target_id, relation.kind).second)
                        room_relations.push_back(relation);
                }
            } catch (const DocumentError&) {
                throw;
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity,
                               "invalid room relationships entity " + id + ": " + error.what());
            }
        }
    }
    // Separate records are one semantic graph. Deduplicate exact definitions,
    // then check physical aliases, cross-record cycles and driver conflicts.
    try {
        std::vector<RoomReference> references;
        references.reserve(room_references.size());
        for (const auto& [id, reference] : room_references) {
            (void)id;
            references.push_back(reference);
        }
        (void)RoomRelationshipSnapshot::create(std::move(references), std::move(room_relations));
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity,
            "Invalid combined room relationship graph: " + std::string(error.what()));
    }
    std::optional<std::string> unsupported_boundary = std::move(unsupported_relationship);
    for(const auto& [id,entity]:entities)
        if(entity.type=="wall" && entity.extensions.contains("wall_split_archive") &&
            entity.extensions.at("wall_split_archive").at("version")!=1)
            unsupported_boundary="Unsupported wall split archive: "+id;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "measurement_boundary" || !entity.extensions.contains("survey_source")) continue;
        const auto admission = inspect_survey_source(entity.extensions.at("survey_source"));
        if (admission.unsupported) {
            unsupported_boundary = *admission.unsupported + ": " + id;
            break;
        }
    }
    // Group provenance has its own reader contract. Retain unknown payloads
    // verbatim, including historical records, without authorizing mutations.
    for(const auto& [id,entity]:entities) {
        if(entity.type!="measurement_boundary" || !entity.extensions.contains("measurement_linework_group"))continue;
        const auto& group=entity.extensions.at("measurement_linework_group");
        if(!group.is_object() || group.size()!=2 || !group.contains("version") ||
            !group.at("version").is_number_integer() || group.at("version")!=1 ||
            !group.contains("members") || !group.at("members").is_array()) {
            unsupported_boundary="Unsupported combined measured-area provenance: "+id;
            break;
        }
    }
    try {
        const auto unsupported_geometry=validate_boundary_integrity(entities);
        if(!unsupported_boundary)unsupported_boundary=unsupported_geometry;
    }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    try {
        const auto unsupported_linework = validate_measurement_linework_integrity(entities);
        if (!unsupported_boundary) unsupported_boundary = unsupported_linework;
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    try {
        const auto unsupported_constraint = validate_constraint_integrity(entities);
        return unsupported_boundary ? unsupported_boundary : unsupported_constraint;
    }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::constraint_violation, error.what());
    }
}

void validate_constraint_change(const std::map<std::string, Entity, std::less<>>& before,
                                const std::map<std::string, Entity, std::less<>>& after,
                                bool qualified_curve_edits=false,
                                bool validate_curve_provenance=true,
                                bool qualified_rigid_transform=false,
                                const std::set<std::string,std::less<>>& verified_rigid_wall_ids={}) {
    try {
        validate_constraint_transition(before, after, qualified_rigid_transform, verified_rigid_wall_ids);
        if (validate_curve_provenance)
            validate_constraint_wall_geometry_transition(before,after,qualified_curve_edits);
    }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::constraint_violation, error.what());
    }
}

void validate_boundary_change(const BoundaryIdentityHistory& history,
                              const std::map<std::string, Entity, std::less<>>& before,
                              const std::map<std::string, Entity, std::less<>>& after,
                              bool allow_explicit_relationship_transform = false) {
    try {
        validate_boundary_transition(before, after, allow_explicit_relationship_transform);
        validate_boundary_identity_transition(history, before, after);
    }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
}

void validate_expected_revision(Revision actual, Revision expected) {
    if (actual != expected) {
        document_error(DocumentErrorCode::stale_revision,
                       "command expected revision " + std::to_string(expected) +
                           " but document is at revision " + std::to_string(actual));
    }
}

std::string command_message(const ApplyEntityChanges& command) {
    return command.message.empty() ? "apply entity changes" : command.message;
}

void validate_action(std::string_view action) {
    if (action.empty() || action.size() > 1024 || !is_valid_utf8_without_nul(action)) {
        document_error(DocumentErrorCode::invalid_entity,
                       "revision action must contain 1 to 1024 bytes of valid UTF-8");
    }
}

void validate_revision_name(std::string_view name) {
    if (name.empty() || name.size() > 256 || !is_valid_utf8_without_nul(name)) {
        document_error(DocumentErrorCode::duplicate_revision_name,
                       "revision name must contain 1 to 256 bytes of valid UTF-8");
    }
}

bool same_state(const RevisionRecord& left, const RevisionRecord& right) {
    if (left.entities != right.entities || left.assets != right.assets) return false;
    // JSON numeric equality treats 1 and 1.0 (and signed zero) alike. Exact
    // navigation must also preserve their serialized form, including opaque
    // metadata; signed/unsigned integer storage types both serialize as JSON 1.
    for (const auto& [id, entity] : left.entities) {
        const auto& other = right.entities.at(id);
        if (entity.properties.dump() != other.properties.dump() ||
            entity.extensions.dump() != other.extensions.dump()) return false;
    }
    for (const auto& [id, asset] : left.assets) {
        if (asset.metadata.dump() != right.assets.at(id).metadata.dump()) return false;
    }
    return true;
}

std::vector<Revision> appended(std::vector<Revision> revisions, Revision revision) {
    revisions.push_back(revision);
    return revisions;
}

}  // namespace

DocumentError::DocumentError(DocumentErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

DocumentErrorCode DocumentError::code() const noexcept { return code_; }

std::string make_stable_id() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(random());
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result << '-';
        }
        result << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return result.str();
}

std::string sha256_hex(std::span<const std::byte> bytes) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;

    auto check = [](NTSTATUS status, std::string_view operation) {
        if (status < 0) {
            throw std::runtime_error(std::string("BCrypt SHA-256 ") + std::string(operation) +
                                     " failed");
        }
    };
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
              "initialization");
        check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                &received, 0),
              "object-size query");
        check(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &received,
                                0),
              "digest-size query");
        object.resize(object_size);
        digest.resize(hash_size);
        check(BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0),
              "hash creation");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(
                remaining, static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));
            check(BCryptHashData(hash,
                                 reinterpret_cast<PUCHAR>(
                                     const_cast<std::byte*>(bytes.data() + offset)),
                                 chunk, 0),
                  "update");
            offset += chunk;
        }
        check(BCryptFinishHash(hash, digest.data(), hash_size, 0), "finalization");
    } catch (...) {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = hex[digest[index] >> 4U];
        result[index * 2 + 1] = hex[digest[index] & 0x0fU];
    }
    return result;
#else
    (void)bytes;
    throw std::runtime_error("SHA-256 requires Windows BCrypt in this build");
#endif
}

bool is_known_entity_type(std::string_view type) noexcept {
    static constexpr std::array<std::string_view, 36> known{
        "property",             "building", "floor",  "layer", "boundary",
        "measurement_boundary", "room_boundary", "wall", "opening", "room",
        "slab",                 "roof",     "stair",  "railing", "column", "beam",
        "label",                "sheet",    "view",   "constraint", "dimension",
        "sheet_view_model",    "annotation_state", "reference_asset",
        "assembly_model",      "model_phases", "room_relationships", "vertical_levels",
        "reference_grid", "terrain_surface", "dxf_source", "ifc_source",
        "georeferencing", "wall_join", "roof_join", "measurement_linework"};
    return std::find(known.begin(), known.end(), type) != known.end();
}

Entity Entity::create(std::string type, nlohmann::json properties, bool required,
                      nlohmann::json extensions) {
    return Entity{make_stable_id(), std::move(type), std::move(properties), required,
                  std::move(extensions)};
}

Asset Asset::create(std::string id, std::string media_type, std::vector<std::byte> bytes,
                    nlohmann::json metadata) {
    const auto digest = sha256_hex(bytes);
    return Asset{std::move(id), std::move(media_type), std::move(bytes), digest,
                 std::move(metadata)};
}

Asset Asset::create(std::string media_type, std::vector<std::byte> bytes,
                    nlohmann::json metadata) {
    return create(make_stable_id(), std::move(media_type), std::move(bytes), std::move(metadata));
}

static bool has_supplemental_source_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.supplemental_source_completion || command.supplemental_asset_reference_completion ||
        !command.supplemental_entity_changes.empty() ||
        !command.supplemental_asset_changes.empty();
}

static bool has_rigid_wall_transform(const ApplyBoundaryConstraintChanges& command) {
    return command.rigid_wall_transform_completion || std::any_of(command.wall_edits.begin(),command.wall_edits.end(),
        [](const auto& edit){return edit.version==4;});
}

static bool has_measured_source_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.measured_source_completion || !command.measured_stroke_edits.empty();
}

static void validate_exterior_resize_related_edits(const ApplyBoundaryConstraintChanges& command) {
    if (command.exterior_segment_resize && !command.exterior_segment_resize->move_connected_objects &&
        (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.measured_stroke_edits.empty()))
        throw std::invalid_argument("Exterior segment resize with frozen related objects cannot carry dependent geometry edits");
    if (command.exterior_segment_arc && !command.exterior_segment_arc->move_connected_objects &&
        (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.measured_stroke_edits.empty()))
        throw std::invalid_argument("Exterior segment arc with frozen related objects cannot carry dependent geometry edits");
}

static void validate_measured_stroke_edit(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& edit) {
    if(!is_valid_identifier(edit.stroke_id) || (edit.authored_edit && edit.rigid_transform) ||
       (edit.authored_length && (!edit.authored_edit || edit.authored_edit->kind!=BoundaryGeometryEditKind::resize_segment)) ||
       (!edit.authored_edit && !edit.rigid_transform && edit.vertex_edits.empty()))
        throw std::invalid_argument("Measured stroke proof has incompatible or empty intent");
    if(edit.authored_length)(void)encode_constraint_quantity_receipt(*edit.authored_length);
    const auto validate=[&](const BoundaryGeometryEdit& value,bool vertex_only) {
        validate_boundary_geometry_edit(value);
        if(value.boundary_id!=edit.stroke_id ||
           (value.kind!=BoundaryGeometryEditKind::move_vertex &&
            (vertex_only || value.kind!=BoundaryGeometryEditKind::resize_segment)))
            throw std::invalid_argument("Measured stroke proof requires same-owner endpoint edits");
    };
    if(edit.authored_edit)validate(*edit.authored_edit,false);
    std::set<std::string,std::less<>> vertices;
    for(const auto& value:edit.vertex_edits) {
        validate(value,true);
        if(!vertices.insert(value.target_id).second)throw std::invalid_argument("Measured stroke proof repeats a vertex");
    }
}

static Entity replay_measured_stroke_edit(const Entity& source,
    const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& edit) {
    validate_measured_stroke_edit(edit);
    if(source.id!=edit.stroke_id || source.type!="measurement_linework")
        throw std::invalid_argument("Measured stroke proof owner does not exist");
    const auto decoded=decode_measurement_linework_model(source.properties.at("model"));
    if(!decoded.supported())throw std::invalid_argument(decoded.diagnostic);
    auto model=*decoded.model;
    if(edit.authored_edit)model=edited_measurement_linework(model,*edit.authored_edit,edit.authored_length);
    if(edit.rigid_transform)model=transformed_measurement_linework(model,*edit.rigid_transform);
    if(!edit.vertex_edits.empty())model=edited_measurement_linework_vertices(model,edit.vertex_edits);
    auto result=source;result.properties["model"]=encode_measurement_linework_model(model);
    return result;
}

static bool exact_entity_payload(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}

static bool exact_asset_payload(const Asset& left, const Asset& right) {
    return left == right && left.metadata.dump() == right.metadata.dump();
}

static bool v6_physical_wall_properties_supported(const Entity& previous, const Entity& wall) {
    auto unchanged = wall.properties;
    for (const auto* key : {"baseline", "thickness_m", "thickness", "height_m", "height", "layers", "classification", "name"}) {
        unchanged.erase(key);
        if (previous.properties.contains(key)) unchanged[key] = previous.properties.at(key);
    }
    return unchanged == previous.properties;
}

static bool v6_physical_wall_extensions_supported(const Entity& previous, const Entity& wall) {
    auto retained = wall.extensions;
    for (const auto* key : {"curve_input", "curve_input_derivation", "constraint_authoring"}) {
        retained.erase(key);
        if (previous.extensions.contains(key)) retained[key] = previous.extensions.at(key);
    }
    return retained == previous.extensions;
}

static bool has_dimension_placement_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.dimension_placement_completion || !command.dimension_placement_moves.empty();
}

bool has_exterior_source_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.exterior_source_completion || !command.physical_entity_changes.empty() ||
        !command.exterior_source_edits.empty() || (!has_rigid_wall_transform(command) &&
            !has_measured_source_completion(command) && !has_dimension_placement_completion(command) &&
            has_supplemental_source_completion(command)) ||
        command.exterior_corner_move.has_value() || command.exterior_segment_resize.has_value() ||
        command.exterior_segment_arc.has_value();
}

static void validate_dimension_placement_intent(const ApplyBoundaryConstraintChanges& command, bool admission) {
    if (!has_dimension_placement_completion(command)) return;
    if (!command.dimension_placement_completion)
        throw std::invalid_argument("Dimension placement lane requires its explicit completion mode");
    if (command.wall_split || command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc)
        throw std::invalid_argument("Dimension placement cannot borrow split, corner, resize or arc intent authority");
    if (admission && (command.dimension_placement_moves.empty() || command.wall_edits.empty()))
        throw std::invalid_argument("Dimension placement completion requires explicit moves and a typed wall proof");
    std::set<std::string, std::less<>> ids;
    for (const auto& move : command.dimension_placement_moves) {
        if (!is_valid_identifier(move.dimension_id) || !std::isfinite(move.offset.x) || !std::isfinite(move.offset.y))
            throw std::invalid_argument("Dimension placement requires a valid saved ID and finite offset");
        if (!ids.insert(move.dimension_id).second)
            throw std::invalid_argument("Dimension placement repeats a saved callout");
        for (const auto* lane : {&command.entity_changes, &command.physical_entity_changes, &command.supplemental_entity_changes})
            for (const auto& change : *lane)
                if ((change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id) == move.dimension_id)
                    throw std::invalid_argument("Dimension placement overlaps an explicit raw entity change");
    }
}

static void complete_dimension_placements(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const ApplyBoundaryConstraintChanges& command) {
    for (const auto& move : command.dimension_placement_moves) {
        const auto original = source.find(move.dimension_id);
        const auto current = candidate.find(move.dimension_id);
        if (original == source.end() || current == candidate.end() || original->second.type != current->second.type ||
            original->second.required != current->second.required)
            throw std::invalid_argument("Dimension placement requires the same surviving saved callout");
        const auto before = decode_boundary_dimension_entity(original->second);
        const auto after = decode_boundary_dimension_entity(current->second);
        if (!before.supported() || !after.supported())
            throw std::invalid_argument("Dimension placement requires supported source and candidate callouts");
        const auto& a = *before.dimension; auto b = *after.dimension;
        if (a.id != b.id || a.boundary_id != b.boundary_id || a.kind != b.kind || a.segment_id != b.segment_id ||
            a.vertex_id != b.vertex_id || a.secondary_segment_id != b.secondary_segment_id || a.segment_chain_ids != b.segment_chain_ids)
            throw std::invalid_argument("Dimension placement cannot remap its stable analytical target");
        const auto old_owner = source.find(a.boundary_id);
        const auto new_owner = candidate.find(b.boundary_id);
        if (old_owner == source.end() || new_owner == candidate.end() || old_owner->second.type != new_owner->second.type)
            throw std::invalid_argument("Dimension placement analytical owner must survive with the same type");
        (void)a.resolve(old_owner->second); (void)b.resolve(new_owner->second);
        b.text_position = {a.text_position.x + move.offset.x, a.text_position.y + move.offset.y};
        if (!std::isfinite(b.text_position.x) || !std::isfinite(b.text_position.y))
            throw std::invalid_argument("Dimension placement text position overflows");
        b.placement = BoundaryDimensionPlacement::manual;
        b.automatic_placement_version.reset();
        // Automatic source reflow is an intentional predecessor. Preserve its
        // surviving entity, style and opaque metadata while changing placement.
        current->second = encode_boundary_dimension_entity(b, &current->second);
    }
}

static bool has_measured_stroke_dimensions(const std::map<std::string,Entity,std::less<>>& source,
    const std::string& owner) {
    return std::any_of(source.begin(),source.end(),[&](const auto& entry) {
        const auto& entity=entry.second;
        if(!can_recognize_boundary_dimension_entity_type(entity.type))return false;
        const auto target=entity.properties.find("target");
        return target!=entity.properties.end() && target->is_object() &&
            target->value("entity_id",nlohmann::json())==owner;
    });
}

static void complete_measured_stroke_dimensions(const std::map<std::string,Entity,std::less<>>& source,
    std::map<std::string,Entity,std::less<>>& entities,
    const std::map<std::string,PlanarTransform,std::less<>>& transforms) {
    if(transforms.empty())return;
    for(const auto& [id,original]:source) {
        if(!can_recognize_boundary_dimension_entity_type(original.type))continue;
        const auto decoded=decode_boundary_dimension_entity(original);
        if(!decoded.supported()) {
            const auto target=original.properties.find("target");
            if(target!=original.properties.end() && target->is_object() && target->contains("entity_id") &&
               target->at("entity_id").is_string() && transforms.contains(target->at("entity_id").get<std::string>()))
                throw std::invalid_argument("Unsupported attached dimension cannot follow a measured transform");
            continue;
        }
        const auto transform=transforms.find(decoded.dimension->boundary_id);
        if(transform==transforms.end())continue;
        const auto candidate=entities.find(id);
        if(candidate==entities.end() || candidate->second!=original)
            throw std::invalid_argument("Measured transform overlaps an edit of its attached dimension");
        auto dimension=*decoded.dimension;
        dimension.text_position=transform_point(dimension.text_position,transform->second);
        candidate->second=encode_boundary_dimension_entity(dimension,&original);
    }
}

static void complete_measured_stroke_annotations(const std::map<std::string,Entity,std::less<>>& source,
    std::map<std::string,Entity,std::less<>>& entities,
    const ApplyBoundaryConstraintChanges& command) {
    std::map<std::string,PlanarTransform,std::less<>> transforms;
    for(const auto& edit:command.measured_stroke_edits)if(edit.rigid_transform) {
        const auto& value=*edit.rigid_transform;
        if(value.rotation_radians==0 && !value.flip_horizontal && !value.flip_vertical)continue;
        transforms.emplace(edit.stroke_id,PlanarTransform{{},value.rotation_radians,value.flip_horizontal,value.flip_vertical,{}});
    }
    if(transforms.empty())return;
    for(const auto& [id,original]:source) {
        if(original.type!=kAnnotationEntityType)continue;
        for(const auto& record:original.properties.at("state").at("overrides")) {
            if(record.at("target_kind")!="area" || !record.contains("plan_label_offset_m"))continue;
            const auto found=transforms.find(record.at("target_id").get<std::string>());
            if(found==transforms.end())continue;
            const auto owner=entities.find(id);
            if(owner==entities.end() || owner->second.type!=kAnnotationEntityType)
                throw std::invalid_argument("Measured transform overlaps removal of its label owner");
            auto& entity=owner->second;validate_annotation_entity(entity);
            auto& overrides=entity.properties.at("state").at("overrides");
            const auto target=std::find_if(overrides.begin(),overrides.end(),[&](const auto& candidate) {
                return candidate.at("target_id")==record.at("target_id") && candidate.at("target_kind")==record.at("target_kind");
            });
            if(target==overrides.end() || !target->contains("plan_label_offset_m") ||
               target->at("plan_label_offset_m")!=record.at("plan_label_offset_m"))
                throw std::invalid_argument("Measured transform overlaps an edit of its label offset");
            const auto& offset=record.at("plan_label_offset_m");
            const auto transformed=transform_point({offset.at(0).get<double>(),offset.at(1).get<double>()},found->second);
            (*target)["plan_label_offset_m"]=nlohmann::json::array({transformed.x,transformed.y});
            validate_annotation_entity(entity);
        }
    }
}

static std::map<std::string, Asset, std::less<>> boundary_constraint_assets(
    const std::map<std::string, Asset, std::less<>>& source,
    const ApplyBoundaryConstraintChanges& command) {
    auto result = source;
    std::unordered_set<std::string> touched;
    for (const auto& change : command.supplemental_asset_changes) {
        if (change.kind != AssetChangeKind::upsert && change.kind != AssetChangeKind::erase)
            document_error(DocumentErrorCode::invalid_asset, "Invalid supplemental asset change kind");
        const auto& id = change.kind == AssetChangeKind::upsert ? change.asset.id : change.asset_id;
        if (!touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change, "Asset supplement is changed more than once: " + id);
        if (change.kind == AssetChangeKind::upsert) {
            validate_asset(change.asset);
            result.insert_or_assign(id, change.asset);
        } else {
            if (!is_valid_identifier(id))
                document_error(DocumentErrorCode::invalid_asset, "Deleted supplemental asset ID is invalid");
            result.erase(id);
        }
    }
    return result;
}

std::map<std::string, Entity, std::less<>> replay_retained_wall_split(
    const std::map<std::string, Entity, std::less<>>& source, const WallSplitIntent& intent) {
    // Restoration may retain future models that cannot lend split authority.
    // Replay every understood proof against detached known data, then restore
    // opaque bytes before the caller's complete expected/actual comparison.
    // Authored operations continue through the strict, unfiltered replay path.
    auto known = source;
    std::map<std::string, Entity, std::less<>> opaque;
    for (const auto& [id, entity] : source) {
        if (entity.type == "room_relationships" &&
            room_relationship_model_version(entity.properties.at("model")) > 2) {
            opaque.emplace(id, entity);
            known.erase(id);
        }
    }
    auto expected = replayed_wall_split_entities(known, intent);
    for (auto& [id, entity] : opaque) {
        if (!expected.emplace(id, std::move(entity)).second)
            throw std::invalid_argument("Retained wall split collides with an opaque relationship entity");
    }
    return expected;
}

void validate_completed_constraint_change(const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false) {
    try { validate_exterior_resize_related_edits(command); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation,error.what()); }
    if(command.wall_split) {
        const auto expected = retained_replay ? replay_retained_wall_split(before, *command.wall_split) :
            replayed_wall_split_entities(before, *command.wall_split);
        if(expected!=after)document_error(DocumentErrorCode::constraint_violation,"Wall split differs from complete source reconstruction");
        const auto normalized=wall_split_validation_source(before,expected,*command.wall_split);
        validate_constraint_change(normalized,after,true,true,false);
        return;
    }
    std::set<std::string,std::less<>> rigid_ids;
    for(const auto& edit:command.wall_edits)if(edit.version==4) {
        const auto found=before.find(edit.wall_id);
        if(found==before.end() || !after.contains(edit.wall_id) ||
            !exact_entity_payload(replay_constraint_wall_edit(found->second,edit),after.at(edit.wall_id)))
            document_error(DocumentErrorCode::constraint_violation,"Rigid wall proof differs from independently reconstructed source");
        rigid_ids.insert(edit.wall_id);
    }
    if (!has_exterior_source_completion(command)) {
        validate_constraint_change(before, after, true, true, false, rigid_ids);
        return;
    }
    try {
        validate_constraint_transition(before, after, false, rigid_ids);
        // Typed endpoint authority applies only to its explicitly proved
        // walls. Independently constructed ordinary curves must retain the
        // ordinary reconstruction path, including archived input operations.
        std::set<std::string> typed_ids;
        for (const auto& edit : command.wall_edits) typed_ids.insert(edit.wall_id);
        auto ordinary_before = before, typed_before = before;
        if (command.exterior_corner_move) {
            const auto reconstructed = exterior_corner_physical_entities(before, *command.exterior_corner_move);
            const auto source_ids = exterior_corner_perimeter_ids(before,before.at(command.exterior_corner_move->boundary_id));
            for (const auto& id : source_ids) {
                    const auto& entity = reconstructed.at(id);
                    if (!after.contains(id) || after.at(id) != entity)
                        throw std::invalid_argument("Exterior corner proof differs from independently reconstructed physical source walls");
                    ordinary_before.erase(id);
                    typed_before.erase(id);
                }
        }
        if (command.exterior_segment_resize) {
            const auto reconstructed = exterior_segment_resize_physical_entities(before, *command.exterior_segment_resize);
            const auto source_ids = exterior_corner_perimeter_ids(before,before.at(command.exterior_segment_resize->boundary_id));
            for (const auto& id : source_ids) {
                if (!after.contains(id) || !exact_entity_payload(after.at(id), reconstructed.at(id)))
                    throw std::invalid_argument("Exterior segment resize differs from independently reconstructed physical source walls");
                ordinary_before.erase(id);
                typed_before.erase(id);
            }
            validate_exterior_segment_resize_result(before, after, *command.exterior_segment_resize);
        }
        if (command.exterior_segment_arc) {
            const auto reconstructed = exterior_segment_arc_physical_entities(before, *command.exterior_segment_arc);
            const auto source_ids = exterior_corner_perimeter_ids(before,before.at(command.exterior_segment_arc->boundary_id));
            for (const auto& id : source_ids) {
                if (!after.contains(id) || !exact_entity_payload(after.at(id), reconstructed.at(id)))
                    throw std::invalid_argument("Exterior segment arc differs from independently reconstructed physical source walls");
                ordinary_before.erase(id);
                typed_before.erase(id);
            }
            validate_exterior_segment_arc_result(before, after, *command.exterior_segment_arc);
        }
        for (const auto& [id, entity] : before) {
            if (entity.type != "wall") continue;
            if (typed_ids.contains(id)) ordinary_before.erase(id);
            else typed_before.erase(id);
        }
        validate_constraint_wall_geometry_transition(ordinary_before, after, false);
        validate_constraint_wall_geometry_transition(typed_before, after, true);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::constraint_violation, error.what());
    }
}

static void validate_split_dimension_lifetime(const BoundaryGeometryEdit& edit,
                                       const std::vector<RevisionRecord>& history,
                                       std::size_t preceding_records) {
    auto ids = edit.replacement_dimension_ids;
    if (edit.kind == BoundaryGeometryEditKind::insert_vertex && !edit.new_dimension_id.empty())
        ids.push_back(edit.new_dimension_id);
    for (const auto& id : ids)
        for (std::size_t i = 0; i < preceding_records; ++i)
            if (history[i].entities.contains(id))
                throw std::invalid_argument("Boundary edit dimension ID was already used in retained history");
}

static void validate_exterior_source_redraw(const BoundaryGeometryEdit& edit) {
    validate_boundary_geometry_edit(edit);
    if (edit.kind != BoundaryGeometryEditKind::redefine_boundary || edit.replacement_wall_source_ids.empty() ||
        edit.fresh_topology || !edit.replacement_authoring.is_null() || !edit.replacement_properties.empty() ||
        !edit.replacement_dimension_ids.empty() || !edit.replacement_child_mapping.empty() ||
        !edit.replacement_removed_reference_ids.empty())
        throw std::invalid_argument("Automatic exterior source updates require retained-topology version-three redraws");
}

static void validate_wall_split_lifetime(const WallSplitIntent& intent,
    const std::vector<RevisionRecord>& history,std::size_t preceding_records) {
    std::set<std::string> fresh{intent.second_wall_id,intent.seam_constraint_id};
    for(const auto& owner:intent.measured_owners) {
        if(!fresh.insert(owner.vertex_id).second || !fresh.insert(owner.segment_id).second ||
            (!owner.automatic_dimension_id.empty() && !fresh.insert(owner.automatic_dimension_id).second))
            throw std::invalid_argument("Wall split fresh identities overlap");
    }
    for(std::size_t i=0;i<preceding_records;++i)for(const auto& [id,entity]:history[i].entities) {
        if(fresh.contains(id))throw std::invalid_argument("Wall split identity was already used in retained history: "+id);
        if(!can_recognize_boundary_entity_type(entity.type) ||
            inspect_boundary_entity_version(entity).format!=BoundaryEntityFormat::identified_v1)continue;
        for(const auto& edge:decode_identified_boundary_entity(entity).segments)
            if(fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                throw std::invalid_argument("Wall split child identity was already used in retained history: "+id);
    }
}

std::map<std::string, Entity, std::less<>> boundary_constraint_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false) {
    try { validate_exterior_resize_related_edits(command); validate_dimension_placement_intent(command, true); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    if(command.wall_split) {
        (void)command_to_json(Command{command});
        try{return retained_replay ? replay_retained_wall_split(source, *command.wall_split) :
            replayed_wall_split_entities(source,*command.wall_split);}
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    const bool source_completion = has_exterior_source_completion(command);
    const bool measured_completion=has_measured_source_completion(command);
    if (source_completion || measured_completion || has_dimension_placement_completion(command)) (void)command_to_json(Command{command});
    if (command.boundary_edits.empty() && command.wall_edits.empty() && !source_completion && !measured_completion)
        document_error(DocumentErrorCode::invalid_entity,
                       "Boundary constraint transaction requires geometry edits");
    auto result = source;
    std::unordered_set<std::string> corner_physical_ids;
    if (command.exterior_corner_move) {
        if (!command.physical_entity_changes.empty() || has_supplemental_source_completion(command))
            document_error(DocumentErrorCode::invalid_entity, "Exterior corner proof cannot carry ordinary physical or asset supplements");
        try {
            const auto reconstructed = exterior_corner_physical_entities(source, *command.exterior_corner_move);
            const auto ids = exterior_corner_perimeter_ids(source,source.at(command.exterior_corner_move->boundary_id));
            for (const auto& id : ids) {
                result.at(id) = reconstructed.at(id);
                corner_physical_ids.insert(id);
            }
        } catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    if (command.exterior_segment_resize) {
        try {
            const auto reconstructed = exterior_segment_resize_physical_entities(source, *command.exterior_segment_resize);
            const auto ids = exterior_corner_perimeter_ids(source,source.at(command.exterior_segment_resize->boundary_id));
            for (const auto& id : ids) {
                result.at(id) = reconstructed.at(id);
                corner_physical_ids.insert(id);
            }
        } catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    if (command.exterior_segment_arc) {
        try {
            const auto reconstructed = exterior_segment_arc_physical_entities(source, *command.exterior_segment_arc);
            const auto ids = exterior_corner_perimeter_ids(source,source.at(command.exterior_segment_arc->boundary_id));
            for (const auto& id : ids) {
                result.at(id) = reconstructed.at(id);
                corner_physical_ids.insert(id);
            }
        } catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    try {
        if (!command.boundary_edits.empty()) result = retained_replay
            ? replayed_boundary_entities_batch(result, command.boundary_edits)
            : edited_boundary_entities_batch(result, command.boundary_edits);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    std::unordered_set<std::string> touched;
    std::set<std::string,std::less<>> rigid_wall_ids;
    touched.insert(corner_physical_ids.begin(), corner_physical_ids.end());
    const bool supplemental_completion = has_supplemental_source_completion(command);
    if (supplemental_completion) {
        // Reserve every primary boundary output, including generated dimensions
        // and removed references. Supplements cannot alter or resurrect them.
        for (const auto& [id, entity] : source) {
            const auto after = result.find(id);
            if (after == result.end() || !exact_entity_payload(entity, after->second)) touched.insert(id);
        }
        for (const auto& [id, entity] : result) {
            (void)entity;
            if (!source.contains(id)) touched.insert(id);
        }
    }
    for (const auto& edit : command.wall_edits) {
        if (!touched.insert(edit.wall_id).second)
            document_error(DocumentErrorCode::duplicate_change,"Wall is changed more than once: " + edit.wall_id);
        const auto previous = source.find(edit.wall_id);
        if (previous == source.end())
            document_error(DocumentErrorCode::invalid_entity,"Wall constraint edit owner does not exist");
        try { result.at(edit.wall_id) = replay_constraint_wall_edit(previous->second, edit); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
        if(edit.version==4)rigid_wall_ids.insert(edit.wall_id);
    }
    for(const auto& edit:command.measured_stroke_edits) {
        if(!touched.insert(edit.stroke_id).second)
            document_error(DocumentErrorCode::duplicate_change,"Measured stroke is changed more than once: "+edit.stroke_id);
        const auto previous=source.find(edit.stroke_id);
        if(previous==source.end())document_error(DocumentErrorCode::invalid_entity,"Measured stroke proof owner does not exist");
        try {result.at(edit.stroke_id)=replay_measured_stroke_edit(previous->second,edit);}
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    for (const auto& change : command.entity_changes) {
        if (change.kind != EntityChangeKind::upsert && change.kind != EntityChangeKind::erase)
            document_error(DocumentErrorCode::invalid_entity, "Invalid constraint change kind");
        const auto& id = change.kind == EntityChangeKind::upsert
            ? change.entity.id : change.entity_id;
        if (!touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change,
                           "Constraint is changed more than once: " + id);
        const auto previous = source.find(id);
        if (!is_valid_identifier(id) ||
            (previous != source.end() && previous->second.type != "constraint"))
            document_error(DocumentErrorCode::invalid_entity,
                           "Boundary constraint transaction may only change constraints");
        if (change.kind == EntityChangeKind::upsert) {
            if (change.entity.type != "constraint")
                document_error(DocumentErrorCode::invalid_entity,
                               "Boundary constraint transaction may only upsert constraints");
            validate_entity(change.entity);
            result.insert_or_assign(id, change.entity);
        } else {
            if (previous == source.end())
                document_error(DocumentErrorCode::invalid_entity, "Removed constraint does not exist");
            result.erase(id);
        }
    }
    const auto before_ordinary = result;
    if (source_completion) {
        const auto before_physical = result;
        for (const auto& change : command.physical_entity_changes) {
            const auto& wall = change.entity;
            const auto previous = source.find(wall.id);
            if (change.kind != EntityChangeKind::upsert || previous == source.end() ||
                previous->second.type != "wall" || wall.type != "wall" ||
                wall.required != previous->second.required)
                document_error(DocumentErrorCode::invalid_entity,
                    "Exterior physical supplements require existing walls with retained metadata");
            if (!touched.insert(wall.id).second)
                document_error(DocumentErrorCode::duplicate_change,
                    "Exterior physical supplement overlaps another edit: " + wall.id);
            if (!supplemental_completion && !v6_physical_wall_properties_supported(previous->second, wall))
                document_error(DocumentErrorCode::invalid_entity,
                    "Exterior physical supplements contain unsupported wall properties or changed source context");
            if (!supplemental_completion && !v6_physical_wall_extensions_supported(previous->second, wall))
                document_error(DocumentErrorCode::invalid_entity,
                    "Exterior physical supplements must retain unrelated wall extension metadata");
            if (supplemental_completion)
                for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "phase_id"})
                    if (wall.properties.contains(key) != previous->second.properties.contains(key) ||
                        (wall.properties.contains(key) && wall.properties.at(key) != previous->second.properties.at(key)))
                        document_error(DocumentErrorCode::invalid_entity,
                            "Exterior physical supplements must retain original wall source context");
            validate_entity(wall);
            result.at(wall.id) = wall;
        }
        // Ordinary physical properties retain the same admission contracts;
        // typed source authority does not qualify supplemental raw geometry.
        if (!supplemental_completion) {
            validate_constraint_change(before_physical, result);
            try { validate_boundary_transition(before_physical, result); }
            catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
        }
    }
    if (!command.supplemental_entity_changes.empty()) {
        std::unordered_set<std::string> protected_ids;
        for (const auto& edit : command.boundary_edits) protected_ids.insert(edit.boundary_id);
        for (const auto& edit : command.exterior_source_edits) protected_ids.insert(edit.boundary_id);
        for (const auto& [id, entity] : source)
            if ((measured_completion && entity.type=="measurement_linework") || can_recognize_boundary_entity_type(entity.type) ||
                can_recognize_boundary_dimension_entity_type(entity.type)) protected_ids.insert(id);
        for (const auto& [id, entity] : before_ordinary)
            if ((measured_completion && entity.type=="measurement_linework") || can_recognize_boundary_entity_type(entity.type) ||
                can_recognize_boundary_dimension_entity_type(entity.type)) protected_ids.insert(id);
        for (const auto& change : command.supplemental_entity_changes) {
            if (change.kind != EntityChangeKind::upsert && change.kind != EntityChangeKind::erase)
                document_error(DocumentErrorCode::invalid_entity, "Invalid supplemental entity change kind");
            const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id;
            if (!is_valid_identifier(id))
                document_error(DocumentErrorCode::invalid_entity, "Supplemental entity ID is invalid");
            if (protected_ids.contains(id) || (change.kind == EntityChangeKind::upsert &&
                ((measured_completion && change.entity.type=="measurement_linework") || can_recognize_boundary_entity_type(change.entity.type) ||
                 can_recognize_boundary_dimension_entity_type(change.entity.type))))
                document_error(DocumentErrorCode::invalid_entity,
                    "Exterior supplements cannot raw-edit a measured owner or dimension");
            if (!touched.insert(id).second)
                document_error(DocumentErrorCode::duplicate_change,
                    "Exterior supplement overlaps another edit: " + id);
            const auto previous = source.find(id);
            if (change.kind == EntityChangeKind::upsert && previous != source.end() &&
                previous->second.type == "wall")
                document_error(DocumentErrorCode::invalid_entity,
                    "Existing wall changes require the exterior physical or typed wall lane");
            if (change.kind == EntityChangeKind::upsert) {
                validate_entity(change.entity);
                result.insert_or_assign(id, change.entity);
            } else result.erase(id);
        }
    }
    if (supplemental_completion) {
        // Physical changes and ordinary relationship edits are admitted
        // together, so a removed lock cannot reject an intermediate wall
        // state that the complete ordinary transaction admits. This never
        // widens typed authority: primary typed geometry is already present
        // in before_ordinary, and raw owner/dimension edits were rejected.
        validate_constraint_change(before_ordinary, result);
        try { validate_boundary_transition(before_ordinary, result); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    for (const auto& edit : command.wall_edits) {
        try { validate_constraint_wall_host(edit.wall_id, result); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    try {
        if (command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc) validate_exterior_corner_edit_topology(source,result);
        else validate_constraint_edit_topology(source,result,rigid_wall_ids);
    }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    if (source_completion) {
        try {
            if (command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc) validate_exterior_corner_physical_contacts(source, result);
            if (command.exterior_source_edits.empty())
                throw std::invalid_argument("Exterior source completion requires explicit redraws");
            const auto expected = exterior_wall_measurement_source_updates(source, result);
            if (expected != command.exterior_source_edits)
                throw std::invalid_argument("Exterior source redraws differ from complete physical-wall lineage reconstruction");
            for (const auto& update : expected)
                if (std::any_of(command.boundary_edits.begin(), command.boundary_edits.end(), [&](const auto& edit) {
                        return edit.boundary_id == update.boundary_id;
                    }))
                    throw std::invalid_argument("Exterior source redraw overlaps primary boundary geometry");
            result = retained_replay ? replayed_boundary_entities_batch(result, expected)
                                     : edited_boundary_entities_batch(result, expected);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity, error.what());
        }
    }
    if(measured_completion) {
        try {
            result=complete_measurement_linework_sources(source,result);
            std::map<std::string,PlanarTransform,std::less<>> transforms;
            for(const auto& edit:command.measured_stroke_edits)
                if(edit.rigid_transform)transforms.emplace(edit.stroke_id,*edit.rigid_transform);
            complete_measured_stroke_dimensions(source,result,transforms);
            complete_measured_stroke_annotations(source,result,command);
        }
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    if (has_dimension_placement_completion(command)) {
        try { complete_dimension_placements(source, result, command); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    if (command.exterior_segment_resize) {
        try { validate_exterior_segment_resize_result(source, result, *command.exterior_segment_resize); }
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    if (command.exterior_segment_arc) {
        try { validate_exterior_segment_arc_result(source, result, *command.exterior_segment_arc); }
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    return result;
}

Command complete_exterior_wall_measurement_command(const DocumentSnapshot& source, const Command& command) {
    const auto* ordinary = std::get_if<ApplyEntityChanges>(&command);
    const auto* constrained = std::get_if<ApplyBoundaryConstraintChanges>(&command);
    if (!ordinary && !constrained) return command;
    if (constrained && has_exterior_source_completion(*constrained)) {
        (void)Document::preview_command(source, command);
        return command;
    }
    if (ordinary && std::none_of(ordinary->entity_changes.begin(), ordinary->entity_changes.end(), [&](const auto& change) {
            if (change.kind != EntityChangeKind::upsert) return false;
            const auto previous = source.entities().find(change.entity.id);
            return previous != source.entities().end() && previous->second.type == "wall" &&
                previous->second != change.entity;
        }))
        // Deletion may also update phase, annotation and sheet registries.
        // Those side effects do not turn it into a physical wall edit. New
        // objects also retain ordinary admission without an extra history fork.
        return command;
    // Ordinary commands retain their normal admission before any source
    // authority is added. Legacy constraint commands retain their typed proof.
    const auto physical = Document::preview_command(source, command);
    const auto updates = exterior_wall_measurement_source_updates(source.entities(), physical.entities());
    if (updates.empty()) return command;
    ApplyBoundaryConstraintChanges completed = constrained ? *constrained : ApplyBoundaryConstraintChanges{};
    completed.expected_revision = source.revision();
    completed.exterior_source_completion = true;
    completed.exterior_source_edits = updates;
    if (ordinary) {
        completed.message = ordinary->message;
        for (const auto& change : ordinary->asset_changes) {
            const auto& id = change.kind == AssetChangeKind::upsert ? change.asset.id : change.asset_id;
            const auto previous = source.assets().find(id);
            if (change.kind == AssetChangeKind::upsert && previous != source.assets().end() &&
                exact_asset_payload(previous->second, change.asset)) continue;
            if (change.kind == AssetChangeKind::erase && previous == source.assets().end()) continue;
            completed.supplemental_asset_changes.push_back(change);
        }
        completed.supplemental_asset_reference_completion = !completed.supplemental_asset_changes.empty();
        const auto read = [](const Entity& wall) {
            const auto& b = wall.properties.at("baseline");
            return Segment{{b.at("start")[0].get<double>(), b.at("start")[1].get<double>()},
                {b.at("end")[0].get<double>(), b.at("end")[1].get<double>()}, b.value("sweep_radians", 0.0)};
        };
        for (const auto& change : ordinary->entity_changes) {
            const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id;
            const auto previous = source.entities().find(id);
            if (change.kind == EntityChangeKind::upsert && previous != source.entities().end() &&
                exact_entity_payload(previous->second, change.entity)) continue; // Exact no-op payloads carry no intent.
            if (change.kind != EntityChangeKind::upsert || previous == source.entities().end() ||
                previous->second.type != "wall" || change.entity.type != "wall") {
                completed.supplemental_entity_changes.push_back(change);
                continue;
            }
            bool typed = false;
            if (previous->second.properties.at("baseline") != change.entity.properties.at("baseline")) {
                const auto old = read(previous->second), next = read(change.entity);
                if (old.sweep_radians == next.sweep_radians) {
                    ConstraintWallGeometryEdit edit{id, next, std::nullopt, old.sweep_radians == 0.0 ? 1ULL : 2ULL};
                    try {
                        if (exact_entity_payload(replay_constraint_wall_edit(previous->second, edit), change.entity)) {
                            completed.wall_edits.push_back(std::move(edit)); typed = true;
                        }
                    } catch (const std::invalid_argument&) {
                        // Explicit ordinary construction retains its ordinary
                        // provenance admission through the physical lane.
                    }
                }
            }
            if (!typed) {
                completed.physical_entity_changes.push_back(change);
                if (!v6_physical_wall_properties_supported(previous->second, change.entity) ||
                    !v6_physical_wall_extensions_supported(previous->second, change.entity))
                    completed.supplemental_source_completion = true;
            }
        }
        completed.supplemental_source_completion = completed.supplemental_source_completion ||
            !completed.supplemental_entity_changes.empty() ||
            !completed.supplemental_asset_changes.empty();
    }
    const Command result{std::move(completed)};
    const auto verified = Document::preview_command(source, result);
    auto expected = physical.history().back();
    expected.entities = edited_boundary_entities_batch(physical.entities(), updates);
    if (!same_state(verified.history().back(), expected))
        document_error(DocumentErrorCode::invalid_entity,
            "Automatic exterior completion does not reproduce the exact authored physical command");
    return result;
}

namespace {

std::map<std::string, Entity, std::less<>> boundary_translation_entities(
    const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& source,
    const TranslateBoundaries& command) {
    if (command.translations.empty())
        document_error(DocumentErrorCode::invalid_entity, "Boundary translation group is empty");
    std::unordered_set<std::string> protected_ids;
    for (const auto& translation : command.translations) {
        if (!is_valid_identifier(translation.boundary_id) ||
            !std::isfinite(translation.offset.x) || !std::isfinite(translation.offset.y))
            document_error(DocumentErrorCode::invalid_entity, "Boundary translation is invalid");
        if (!protected_ids.insert(translation.boundary_id).second)
            document_error(DocumentErrorCode::duplicate_change, "Boundary is translated more than once");
    }
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (decoded.dimension && protected_ids.contains(decoded.dimension->boundary_id))
            protected_ids.insert(id);
    }
    auto intermediate = source;
    try {
        for (const auto& translation : command.translations)
            // Keep original analytical starts in their local construction
            // frame. Adding a world offset to each old start independently can
            // change floating-point joins for exact imperial rise/run entries.
            // Historical single-translation proofs keep their old replay path.
            intermediate = transformed_boundary_entities(intermediate,
                BoundaryTransformation{translation.boundary_id,
                    PlanarTransform{{},0,false,false,translation.offset}});
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    auto result = intermediate;
    std::unordered_set<std::string> touched;
    std::map<std::string,PlanarTransform,std::less<>> measured_transforms;
    for (const auto& change : command.entity_changes) {
        if (change.kind != EntityChangeKind::upsert && change.kind != EntityChangeKind::erase)
            document_error(DocumentErrorCode::invalid_entity, "Invalid supplemental entity change kind");
        const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id;
        if (protected_ids.contains(id) || !touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change,
                           "Supplemental change overlaps a translated owner or dimension: " + id);
        if (!is_valid_identifier(id))
            document_error(DocumentErrorCode::invalid_entity, "Supplemental entity ID is invalid");
        if (change.kind == EntityChangeKind::upsert) {
            validate_entity(change.entity);
            const auto previous=source.find(id);
            if(previous!=source.end() && previous->second.type=="measurement_linework" &&
               has_measured_stroke_dimensions(source,id)) {
                const auto offset=command.translations.front().offset;
                if(!std::all_of(command.translations.begin(),command.translations.end(),[&](const auto& value) {
                    return value.offset.x==offset.x && value.offset.y==offset.y;
                }))throw std::invalid_argument("Measured dimensions require one shared translation for a mixed group");
                const PlanarTransform transform{{},0,false,false,offset};
                const auto decoded=decode_measurement_linework_model(previous->second.properties.at("model"));
                if(!decoded.supported())throw std::invalid_argument(decoded.diagnostic);
                auto expected=previous->second;
                expected.properties["model"]=encode_measurement_linework_model(transformed_measurement_linework(*decoded.model,transform));
                if(expected!=change.entity)throw std::invalid_argument("Measured stroke differs from the shared translation");
                measured_transforms.emplace(id,transform);
            }
            result.insert_or_assign(id, change.entity);
        } else result.erase(id);
    }
    // Supplemental edits get ordinary admission against the translated state;
    // they cannot use the typed proof to launder an unrelated receipt edit.
    complete_measured_stroke_dimensions(source,result,measured_transforms);
    validate_boundary_change(history, intermediate, result);
    try { validate_boundary_identity_transition(history, source, result); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    return result;
}

Boundary uniquely_aligned_rigid_exterior(const Boundary& expected, const Boundary& derived,
                                        const PlanarTransform& transform) {
    if (expected.size() != derived.size())
        throw std::invalid_argument("Rigid exterior transform changed analytical topology");
    const auto within_roundoff = [&](double a, double b, bool coordinate) {
        double scale = std::max({1.0, std::abs(a), std::abs(b)});
        if (coordinate)
            scale = std::max({scale, std::abs(transform.pivot.x), std::abs(transform.pivot.y),
                std::abs(transform.offset.x), std::abs(transform.offset.y)});
        return std::abs(a - b) <= 128.0 * std::numeric_limits<double>::epsilon() * scale;
    };
    const auto same = [&](const Segment& a, const Segment& b) {
        return within_roundoff(a.start.x,b.start.x,true) && within_roundoff(a.start.y,b.start.y,true) &&
            within_roundoff(a.end.x,b.end.x,true) && within_roundoff(a.end.y,b.end.y,true) &&
            within_roundoff(a.sweep_radians,b.sweep_radians,false);
    };
    Boundary aligned;
    unsigned matches = 0;
    for (std::size_t offset = 0; offset < derived.size(); ++offset) {
        for (const bool reverse : {false, true}) {
            Boundary candidate;
            bool match = true;
            for (std::size_t i = 0; i < derived.size(); ++i) {
                auto segment = derived[(offset + (reverse ? derived.size() - i : i)) % derived.size()];
                if (reverse) {
                    std::swap(segment.start, segment.end);
                    segment.sweep_radians = -segment.sweep_radians;
                }
                if (!same(expected[i], segment)) {
                    match = false;
                    break;
                }
                candidate.push_back(segment);
            }
            if (match) {
                ++matches;
                aligned = std::move(candidate);
            }
        }
    }
    if (matches != 1)
        throw std::invalid_argument("Rigid exterior requires one unique machine-precision analytical correspondence");
    return aligned;
}

std::map<std::string, Entity, std::less<>> boundary_transform_entities(
    const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& source,
    const TransformBoundaries& command) {
    if (command.transformations.empty())
        document_error(DocumentErrorCode::invalid_entity,"Boundary transform group is empty");
    (void)command_to_json(command); // The retained proof must be persistable too.
    const auto& shared = command.transformations.front().transform;
    std::set<std::string> owners, protected_ids, source_walls, moved_walls;
    for (const auto& transformation : command.transformations) {
        validate_boundary_transform(transformation);
        if (!(transformation.transform == shared))
            document_error(DocumentErrorCode::invalid_entity,"Boundary transform group requires one shared transform");
        if (!owners.insert(transformation.boundary_id).second)
            document_error(DocumentErrorCode::duplicate_change,"Boundary is transformed more than once");
    }
    protected_ids = owners;
    const auto source_current = [&](const auto& entities, const Entity& owner) {
        try {
            const auto ids = exterior_wall_measurement_source_ids(owner);
            const auto derived = derive_exterior_wall_measurement(entities,ids);
            auto recorded = owner.properties.at("wall_measurement_source");
            auto& walls = recorded.at("walls");
            std::sort(walls.begin(), walls.end(), [](const auto& a, const auto& b) {
                return a.at("id").template get<std::string>() < b.at("id").template get<std::string>();
            });
            if (recorded != derived.source)
                return false;
            const auto actual = boundary_geometry(decode_identified_boundary_entity(owner));
            const auto aligned = uniquely_aligned_rigid_exterior(actual,derived.boundary,PlanarTransform{});
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const auto& a = actual[i];
                const auto& b = aligned[i];
                if (a.start.x != b.start.x || a.start.y != b.start.y || a.end.x != b.end.x ||
                    a.end.y != b.end.y || a.sweep_radians != b.sweep_radians)
                    return false;
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    for (const auto& [id, entity] : source) {
        if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto dimension = decode_boundary_dimension_entity(entity);
            if (dimension.dimension && owners.contains(dimension.dimension->boundary_id))
                protected_ids.insert(id);
        }
    }
    for (const auto& id : owners) {
        const auto& owner = source.at(id);
        if (const auto deductions = owner.properties.find("deduction_ids"); deductions != owner.properties.end()) {
            if (!deductions->is_array())
                throw std::invalid_argument("Measured deductions must be an array");
            std::set<std::string> unique;
            for (const auto& value : *deductions) {
                if (!value.is_string() || !owners.contains(value.get<std::string>()) || value == id ||
                    !unique.insert(value.get<std::string>()).second)
                    throw std::invalid_argument("Rigid transform must include every retained deduction exactly once");
            }
        }
        if (owner.properties.contains("wall_measurement_source")) {
            const auto ids = exterior_wall_measurement_source_ids(owner);
            // A transform cannot repair a source that was already stale.
            if (!source_current(source,owner))
                throw std::invalid_argument("Exterior transform requires current recorded source context");
            source_walls.insert(ids.begin(),ids.end());
        }
    }
    auto intermediate = transformed_boundary_entities_batch(source,command.transformations);
    auto result = intermediate;
    std::set<std::string> touched;
    std::map<std::string,PlanarTransform,std::less<>> measured_transforms;
    for (const auto& change : command.entity_changes) {
        const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id;
        if (protected_ids.contains(id) || !touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change,"Supplement overlaps a transformed owner or dimension: " + id);
        const auto previous = source.find(id);
        if (change.kind != EntityChangeKind::upsert || previous == source.end() ||
            change.entity.type != previous->second.type || change.entity.required != previous->second.required)
            throw std::invalid_argument("Rigid transform supplements must preserve existing entity identities and types");
        if (can_recognize_boundary_entity_type(change.entity.type) ||
            can_recognize_boundary_dimension_entity_type(change.entity.type))
            throw std::invalid_argument("Rigid transform supplements cannot inject raw boundary or dimension geometry");
        validate_entity(change.entity);
        if (change.entity.type == "wall") {
            const auto read = [](const Entity& wall) {
                const auto& value = wall.properties.at("baseline");
                return Segment{{value.at("start")[0].get<double>(),value.at("start")[1].get<double>()},
                    {value.at("end")[0].get<double>(),value.at("end")[1].get<double>()},value.value("sweep_radians",0.0)};
            };
            const auto expected = transform_segment(read(previous->second), shared);
            const auto actual = read(change.entity);
            if (expected.start.x != actual.start.x || expected.start.y != actual.start.y ||
                expected.end.x != actual.end.x || expected.end.y != actual.end.y || expected.sweep_radians != actual.sweep_radians)
                throw std::invalid_argument("Supplemental wall baseline differs from the shared rigid transform");
            for (const auto* key : {"thickness_m","thickness","height_m","height","elevation_m","elevation",
                                   "property_id","building_id","floor_id","layer_id","phase_id","vertical_placement"}) {
                if (previous->second.properties.contains(key) != change.entity.properties.contains(key) ||
                    (previous->second.properties.contains(key) && previous->second.properties.at(key) != change.entity.properties.at(key)))
                    throw std::invalid_argument("Rigid wall transform must preserve physical dimensions and source context");
            }
            moved_walls.insert(id);
        } else if(change.entity.type=="measurement_linework" && has_measured_stroke_dimensions(source,id)) {
            const auto decoded=decode_measurement_linework_model(previous->second.properties.at("model"));
            if(!decoded.supported())throw std::invalid_argument(decoded.diagnostic);
            auto expected=previous->second;
            expected.properties["model"]=encode_measurement_linework_model(transformed_measurement_linework(*decoded.model,shared));
            if(expected!=change.entity)throw std::invalid_argument("Measured stroke differs from the shared rigid transform");
            measured_transforms.emplace(id,shared);
        } else if (change.entity.type == "opening") {
            // Hosting distances do not change when the host endpoint identities are retained.
            for (const auto* key : {"wall_id", "opening_kind", "offset_m", "offset", "width_m", "width",
                                   "height_m", "height", "sill_m", "sill_height_m", "sill_height",
                                   "property_id", "building_id", "floor_id", "layer_id", "phase_id"}) {
                if (previous->second.properties.contains(key) != change.entity.properties.contains(key) ||
                    (previous->second.properties.contains(key) &&
                     previous->second.properties.at(key) != change.entity.properties.at(key)))
                    throw std::invalid_argument("Rigid opening transform must preserve its host and physical dimensions");
            }
        }
        result.insert_or_assign(id,change.entity);
    }
    const bool identity = shared.rotation_radians == 0.0 && !shared.flip_horizontal && !shared.flip_vertical &&
        shared.offset.x == 0.0 && shared.offset.y == 0.0;
    for (const auto& id : source_walls)
        if (!moved_walls.contains(id) && !identity)
            throw std::invalid_argument("Rigid exterior transform must include every source wall");
    std::set<std::string> transformed_ids = owners;
    transformed_ids.insert(moved_walls.begin(),moved_walls.end());
    for(const auto& [id,transform]:measured_transforms){(void)transform;transformed_ids.insert(id);}
    for (const auto& [id, entity] : source) {
        if (entity.type != "constraint")
            continue;
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.supported())
            throw std::invalid_argument(decoded.unsupported_reason);
        const auto& bindings = decoded.constraint->bindings;
        const bool affected = std::any_of(bindings.begin(),bindings.end(),[&](const auto& value){return transformed_ids.contains(value.owner_id);});
        if (!affected) {
            if (result.at(id) != entity)
                throw std::invalid_argument("Rigid transform cannot rewrite an unrelated constraint");
            continue;
        }
        if (!std::all_of(bindings.begin(),bindings.end(),[&](const auto& value){return transformed_ids.contains(value.owner_id);}))
            throw std::invalid_argument("Rigid transform cannot move only one owner of an external hard relation");
        auto expected = *decoded.constraint;
        if (expected.anchor)
            expected.anchor = transform_point(*expected.anchor, shared);
        if (expected.relation == ConstraintRelationKind::horizontal || expected.relation == ConstraintRelationKind::vertical) {
            if (std::abs(std::remainder(shared.rotation_radians,std::numbers::pi/2)) > 1e-12)
                throw std::invalid_argument("Axis-locked relations require a quarter-turn rigid rotation");
            if (std::llround(shared.rotation_radians/(std::numbers::pi/2)) % 2 != 0)
                expected.relation = expected.relation == ConstraintRelationKind::horizontal ? ConstraintRelationKind::vertical : ConstraintRelationKind::horizontal;
        }
        if (result.at(id) != encode_constraint_entity(expected,&entity))
            throw std::invalid_argument("Rigid transform must preserve constraint endpoint identities, values and metadata");
    }
    // Reject ordinary receipt edits before any canonical source reconciliation.
    complete_measured_stroke_dimensions(source,result,measured_transforms);
    validate_boundary_change(history,intermediate,result);
    if (identity) {
        if (result != source)
            throw std::invalid_argument("Identity rigid transform cannot perform supplemental edits");
        return source;
    }
    for (const auto& id : owners) {
        if (!result.at(id).properties.contains("wall_measurement_source"))
            continue;
        const auto ids = exterior_wall_measurement_source_ids(result.at(id));
        const auto exterior = derive_replacement_exterior_wall_measurement(result,result.at(id),ids);
        auto boundary = decode_identified_boundary_entity(result.at(id));
        const auto aligned = uniquely_aligned_rigid_exterior(boundary_geometry(boundary),exterior.boundary,shared);
        bool exact = true;
        for (std::size_t i = 0; i < aligned.size(); ++i) {
            const auto& a = boundary.segments[i].segment;
            const auto& b = aligned[i];
            exact = exact && a.start.x == b.start.x && a.start.y == b.start.y && a.end.x == b.end.x &&
                a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
            boundary.segments[i].segment = b;
        }
        if (!exact || result.at(id).properties.at("wall_measurement_source") != exterior.source) {
            BoundaryGeometryEdit edit;
            edit.boundary_id = id;
            edit.target_id = id;
            edit.kind = BoundaryGeometryEditKind::redefine_boundary;
            edit.replacement_segments = encode_identified_boundary_entity(boundary).properties.at("segments");
            edit.replacement_wall_source_ids = ids;
            result = edited_boundary_entities(result, edit);
        }
    }
    const auto organization = organize_project(result);
    for (const auto& [id, owner] : source) {
        if (owners.contains(id) || !can_recognize_boundary_entity_type(owner.type))
            continue;
        if (owner.properties.contains("wall_measurement_source")) {
            const auto ids = exterior_wall_measurement_source_ids(owner);
            if (std::any_of(ids.begin(),ids.end(),[&](const auto& wall){return moved_walls.contains(wall);}) &&
                source_current(source,owner) && !source_current(result,result.at(id)))
                throw std::invalid_argument("Rigid transform would stale an unchanged exterior source consumer");
        }
        if (const auto deductions = owner.properties.find("deduction_ids"); deductions != owner.properties.end()) {
            std::vector<Boundary> holes;
            bool affected = false;
            for (const auto& child_id : *deductions) {
                if (!child_id.is_string())
                    throw std::invalid_argument("Measured deduction identifiers are invalid");
                affected = affected || owners.contains(child_id.get<std::string>());
                const auto& child = result.at(child_id.get<std::string>());
                const auto identified = inspect_boundary_entity_version(child).format == BoundaryEntityFormat::identified_v1
                    ? child : upgrade_legacy_boundary_entity(child);
                holes.push_back(boundary_geometry(decode_identified_boundary_entity(identified)));
            }
            if (affected) {
                const auto identified = inspect_boundary_entity_version(owner).format == BoundaryEntityFormat::identified_v1
                    ? owner : upgrade_legacy_boundary_entity(owner);
                if (const auto invalid = validate_boundary_holes(boundary_geometry(decode_identified_boundary_entity(identified)), holes))
                    throw std::invalid_argument("Rigid transform would invalidate an unchanged deduction consumer: " + *invalid);
            }
        }
    }
    for (const auto& id : owners) {
        const auto& owner = result.at(id);
        if (!owner.properties.contains("deduction_ids"))
            continue;
        const auto context = organization.drawing_context(id);
        std::vector<Boundary> holes;
        for (const auto& child_id : owner.properties.at("deduction_ids")) {
            const auto& child = result.at(child_id.get<std::string>());
            const auto child_context = organization.drawing_context(child.id);
            if (context || child_context) {
                if (!context || !child_context || context->property_id != child_context->property_id ||
                    context->building_id != child_context->building_id || context->floor_id != child_context->floor_id)
                    throw std::invalid_argument("Rigid transform deductions must retain their measured context");
            }
            holes.push_back(boundary_geometry(decode_identified_boundary_entity(child)));
        }
        if (const auto invalid = validate_boundary_holes(boundary_geometry(decode_identified_boundary_entity(owner)), holes))
            throw std::invalid_argument(*invalid);
    }
    validate_boundary_identity_transition(history,source,result);
    return result;
}

void command_exact_fields(const nlohmann::json& value,
                          std::initializer_list<const char*> fields,
                          DocumentErrorCode code, std::string_view context) {
    if (!value.is_object() || value.size() != fields.size()) {
        document_error(code, std::string(context) + " has unexpected fields");
    }
    for (const auto* field : fields) {
        if (!value.contains(field)) {
            document_error(code, std::string(context) + " is missing field " + field);
        }
    }
}

Revision command_revision(const nlohmann::json& value, std::string_view context) {
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && value.get<std::int64_t>() < 0)) {
        document_error(DocumentErrorCode::invalid_entity,
                       std::string(context) + " must be a non-negative integer");
    }
    try {
        return value.get<Revision>();
    } catch (const std::exception&) {
        document_error(DocumentErrorCode::invalid_entity,
                       std::string(context) + " is outside the supported revision range");
    }
}

std::string command_string(const nlohmann::json& value, std::string_view context,
                           std::size_t maximum = 1024) {
    if (!value.is_string()) {
        document_error(DocumentErrorCode::invalid_entity,
                       std::string(context) + " must be a string");
    }
    const auto result = value.get<std::string>();
    if (result.empty() || result.size() > maximum || !is_valid_utf8_without_nul(result)) {
        document_error(DocumentErrorCode::invalid_entity,
                       std::string(context) + " is invalid");
    }
    return result;
}

double command_number(const nlohmann::json& value, std::string_view context) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        document_error(DocumentErrorCode::invalid_entity,
                       std::string(context) + " must be finite");
    }
    return value.get<double>();
}

nlohmann::json command_vec2_to_json(Vec2 point) {
    return nlohmann::json{{"x", point.x}, {"y", point.y}};
}

Vec2 command_vec2_from_json(const nlohmann::json& value, std::string_view context) {
    command_exact_fields(value, {"x", "y"}, DocumentErrorCode::invalid_entity, context);
    return {command_number(value.at("x"), std::string(context) + ".x"),
            command_number(value.at("y"), std::string(context) + ".y")};
}

nlohmann::json command_transform_to_json(const PlanarTransform& transform) {
    return nlohmann::json{
        {"pivot", command_vec2_to_json(transform.pivot)},
        {"rotation_radians", transform.rotation_radians},
        {"flip_horizontal", transform.flip_horizontal},
        {"flip_vertical", transform.flip_vertical},
        {"offset", command_vec2_to_json(transform.offset)},
    };
}

PlanarTransform command_transform_from_json(const nlohmann::json& value) {
    command_exact_fields(value, {"pivot", "rotation_radians", "flip_horizontal",
                                  "flip_vertical", "offset"},
                         DocumentErrorCode::invalid_entity, "command transformation");
    if (!value.at("flip_horizontal").is_boolean() || !value.at("flip_vertical").is_boolean()) {
        document_error(DocumentErrorCode::invalid_entity,
                       "command transformation flip flags must be boolean");
    }
    return {command_vec2_from_json(value.at("pivot"), "command transformation pivot"),
            command_number(value.at("rotation_radians"), "command transformation rotation"),
            value.at("flip_horizontal").get<bool>(), value.at("flip_vertical").get<bool>(),
            command_vec2_from_json(value.at("offset"), "command transformation offset")};
}

nlohmann::json command_measured_stroke_edit_to_json(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& edit) {
    validate_measured_stroke_edit(edit);
    auto vertices=nlohmann::json::array();
    for(const auto& vertex:edit.vertex_edits)vertices.push_back(encode_boundary_geometry_edit(vertex));
    if(edit.rigid_transform)(void)command_transform_from_json(command_transform_to_json(*edit.rigid_transform));
    return {{"stroke_id",edit.stroke_id},
        {"authored_edit",edit.authored_edit ? encode_boundary_geometry_edit(*edit.authored_edit) : nlohmann::json(nullptr)},
        {"authored_length",edit.authored_length ? encode_constraint_quantity_receipt(*edit.authored_length) : nlohmann::json(nullptr)},
        {"rigid_transform",edit.rigid_transform ? command_transform_to_json(*edit.rigid_transform) : nlohmann::json(nullptr)},
        {"vertex_edits",std::move(vertices)}};
}

ApplyBoundaryConstraintChanges::MeasuredStrokeEdit command_measured_stroke_edit_from_json(const nlohmann::json& value) {
    command_exact_fields(value,{"stroke_id","authored_edit","authored_length","rigid_transform","vertex_edits"},
        DocumentErrorCode::invalid_entity,"measured stroke proof");
    if(!value.at("stroke_id").is_string() || !value.at("vertex_edits").is_array())
        throw std::invalid_argument("Measured stroke proof has malformed identity or vertices");
    ApplyBoundaryConstraintChanges::MeasuredStrokeEdit result;result.stroke_id=value.at("stroke_id").get<std::string>();
    if(!value.at("authored_edit").is_null())result.authored_edit=decode_boundary_geometry_edit(value.at("authored_edit"));
    if(!value.at("authored_length").is_null())result.authored_length=decode_constraint_quantity_receipt(value.at("authored_length"));
    if(!value.at("rigid_transform").is_null())result.rigid_transform=command_transform_from_json(value.at("rigid_transform"));
    for(const auto& vertex:value.at("vertex_edits"))result.vertex_edits.push_back(decode_boundary_geometry_edit(vertex));
    validate_measured_stroke_edit(result);return result;
}

std::string command_bytes_to_hex(std::span<const std::byte> bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto byte = std::to_integer<unsigned char>(bytes[index]);
        result[index * 2] = hex[byte >> 4U];
        result[index * 2 + 1] = hex[byte & 0x0fU];
    }
    return result;
}

unsigned char command_hex_digit(char value) {
    if (value >= '0' && value <= '9') return static_cast<unsigned char>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned char>(value - 'a' + 10);
    document_error(DocumentErrorCode::invalid_asset, "serialized asset bytes contain non-hex data");
}

std::vector<std::byte> command_bytes_from_hex(const nlohmann::json& value) {
    if (!value.is_string()) {
        document_error(DocumentErrorCode::invalid_asset, "serialized asset bytes must be a hex string");
    }
    const auto encoded = value.get<std::string>();
    if (encoded.size() % 2 != 0 || encoded.size() > kMaximumAssetBytes * 2) {
        document_error(DocumentErrorCode::invalid_asset, "serialized asset byte string has invalid length");
    }
    std::vector<std::byte> result(encoded.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto high = command_hex_digit(encoded[index * 2]);
        const auto low = command_hex_digit(encoded[index * 2 + 1]);
        result[index] = static_cast<std::byte>((high << 4U) | low);
    }
    return result;
}

nlohmann::json command_entity_to_json(const Entity& entity) {
    validate_entity(entity);
    return nlohmann::json{{"id", entity.id}, {"type", entity.type}, {"required", entity.required},
                          {"properties", entity.properties}, {"extensions", entity.extensions}};
}

Entity command_entity_from_json(const nlohmann::json& value) {
    command_exact_fields(value, {"id", "type", "required", "properties", "extensions"},
                         DocumentErrorCode::invalid_entity, "serialized entity");
    if (!value.at("required").is_boolean() || !value.at("properties").is_object() ||
        !value.at("extensions").is_object()) {
        document_error(DocumentErrorCode::invalid_entity, "serialized entity fields are invalid");
    }
    Entity result{command_string(value.at("id"), "serialized entity id", kMaximumIdBytes),
                  command_string(value.at("type"), "serialized entity type", kMaximumTypeBytes),
                  value.at("properties"), value.at("required").get<bool>(), value.at("extensions")};
    validate_entity(result);
    return result;
}

nlohmann::json command_asset_to_json(const Asset& asset) {
    validate_asset(asset);
    return nlohmann::json{{"id", asset.id}, {"media_type", asset.media_type},
                          {"sha256", asset.sha256}, {"metadata", asset.metadata},
                          {"bytes_hex", command_bytes_to_hex(asset.bytes)}};
}

Asset command_asset_from_json(const nlohmann::json& value) {
    command_exact_fields(value, {"id", "media_type", "sha256", "metadata", "bytes_hex"},
                         DocumentErrorCode::invalid_asset, "serialized asset");
    if (!value.at("metadata").is_object()) {
        document_error(DocumentErrorCode::invalid_asset, "serialized asset metadata must be an object");
    }
    Asset result;
    result.id = command_string(value.at("id"), "serialized asset id", kMaximumIdBytes);
    result.media_type = command_string(value.at("media_type"), "serialized asset media type", 256);
    result.sha256 = command_string(value.at("sha256"), "serialized asset SHA-256", 64);
    result.metadata = value.at("metadata");
    result.bytes = command_bytes_from_hex(value.at("bytes_hex"));
    validate_asset(result);
    return result;
}

std::string command_asset_metadata_sha256(const Asset& asset) {
    const auto canonical = asset.metadata.dump();
    return sha256_hex(std::as_bytes(std::span(canonical.data(), canonical.size())));
}

nlohmann::json command_asset_references_to_json(const std::vector<AssetChange>& changes) {
    auto result = nlohmann::json::array();
    std::unordered_set<std::string> touched;
    for (const auto& change : changes) {
        if (change.kind != AssetChangeKind::upsert && change.kind != AssetChangeKind::erase)
            document_error(DocumentErrorCode::invalid_asset, "Invalid supplemental asset reference kind");
        const auto& id = change.kind == AssetChangeKind::upsert ? change.asset.id : change.asset_id;
        if (!is_valid_identifier(id))
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset reference ID is invalid");
        if (!touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change, "Supplemental asset reference is changed more than once: " + id);
        if (change.kind == AssetChangeKind::erase) {
            result.push_back({{"kind", "erase"}, {"asset_id", id}});
        } else {
            validate_asset(change.asset);
            result.push_back({{"kind", "upsert"}, {"asset", {
                {"id", id}, {"media_type", change.asset.media_type}, {"sha256", change.asset.sha256},
                {"byte_size", change.asset.bytes.size()},
                {"metadata_sha256", command_asset_metadata_sha256(change.asset)}}}});
        }
    }
    return result;
}

std::vector<AssetChange> command_asset_references_from_json(const nlohmann::json& value,
    const std::function<const Asset*(std::string_view)>& resolver) {
    if (!value.is_array())
        document_error(DocumentErrorCode::invalid_asset, "Supplemental asset references must be an array");
    std::vector<AssetChange> result;
    std::unordered_set<std::string> touched;
    for (const auto& change : value) {
        if (!change.is_object() || !change.contains("kind") || !change.at("kind").is_string())
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset reference is invalid");
        const auto kind = change.at("kind").get<std::string>();
        std::string id;
        if (kind == "erase") {
            command_exact_fields(change, {"kind", "asset_id"}, DocumentErrorCode::invalid_asset,
                "supplemental asset reference erase");
            id = command_string(change.at("asset_id"), "supplemental asset reference ID", kMaximumIdBytes);
        } else if (kind == "upsert") {
            command_exact_fields(change, {"kind", "asset"}, DocumentErrorCode::invalid_asset,
                "supplemental asset reference upsert");
            command_exact_fields(change.at("asset"), {"id", "media_type", "sha256", "byte_size", "metadata_sha256"},
                DocumentErrorCode::invalid_asset, "supplemental asset reference");
            id = command_string(change.at("asset").at("id"), "supplemental asset reference ID", kMaximumIdBytes);
        } else {
            document_error(DocumentErrorCode::invalid_asset, "Unknown supplemental asset reference kind");
        }
        if (!is_valid_identifier(id))
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset reference ID is invalid");
        if (!touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change, "Supplemental asset reference is changed more than once: " + id);
        if (kind == "erase") {
            result.push_back(AssetChange::erase(std::move(id)));
            continue;
        }
        const auto& reference = change.at("asset");
        const auto media_type = command_string(reference.at("media_type"), "supplemental asset media type", 256);
        const auto digest = command_string(reference.at("sha256"), "supplemental asset SHA-256", 64);
        const auto metadata_digest = command_string(reference.at("metadata_sha256"), "supplemental asset metadata SHA-256", 64);
        const auto& size = reference.at("byte_size");
        if ((!size.is_number_integer() && !size.is_number_unsigned()) ||
            (size.is_number_integer() && !size.is_number_unsigned() && size.get<std::int64_t>() < 0) ||
            size.get<std::uint64_t>() > kMaximumAssetBytes)
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset byte size is invalid");
        if (!resolver)
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset reference requires an asset resolver");
        const auto* asset = resolver(id);
        if (!asset)
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset reference is missing: " + id);
        validate_asset(*asset);
        if (asset->id != id || asset->media_type != media_type || asset->sha256 != digest ||
            asset->bytes.size() != size.get<std::uint64_t>() || command_asset_metadata_sha256(*asset) != metadata_digest)
            document_error(DocumentErrorCode::invalid_asset, "Supplemental asset reference does not match its resolved payload: " + id);
        result.push_back(AssetChange::upsert(*asset));
    }
    return result;
}

}  // namespace

nlohmann::json command_to_json(const Command& command) {
    return std::visit([](const auto& typed) -> nlohmann::json {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, ApplyEntityChanges>) {
            nlohmann::json entities = nlohmann::json::array();
            for (const auto& change : typed.entity_changes) {
                if (change.kind == EntityChangeKind::upsert) {
                    entities.push_back({{"kind", "upsert"}, {"entity", command_entity_to_json(change.entity)}});
                } else {
                    if (!is_valid_identifier(change.entity_id))
                        document_error(DocumentErrorCode::invalid_entity, "serialized entity erase ID is invalid");
                    entities.push_back({{"kind", "erase"}, {"entity_id", change.entity_id}});
                }
            }
            nlohmann::json assets = nlohmann::json::array();
            for (const auto& change : typed.asset_changes) {
                if (change.kind == AssetChangeKind::upsert) {
                    assets.push_back({{"kind", "upsert"}, {"asset", command_asset_to_json(change.asset)}});
                } else {
                    if (!is_valid_identifier(change.asset_id))
                        document_error(DocumentErrorCode::invalid_asset, "serialized asset erase ID is invalid");
                    assets.push_back({{"kind", "erase"}, {"asset_id", change.asset_id}});
                }
            }
            if (typed.message.size() > 1024 || !is_valid_utf8_without_nul(typed.message))
                document_error(DocumentErrorCode::invalid_entity, "serialized command message is invalid");
            return nlohmann::json{{"version", 1}, {"kind", "apply_entity_changes"},
                                  {"expected_revision", typed.expected_revision}, {"message", typed.message},
                                  {"entity_changes", std::move(entities)}, {"asset_changes", std::move(assets)}};
        } else if constexpr (std::is_same_v<T, TransformBoundaries>) {
            auto encoded = command_to_json(ApplyEntityChanges{
                typed.expected_revision, typed.entity_changes, {}, typed.message});
            encoded["kind"] = "transform_boundaries";
            encoded.erase("asset_changes");
            encoded["transformations"] = nlohmann::json::array();
            if (typed.transformations.empty())
                document_error(DocumentErrorCode::invalid_entity, "Boundary transform group is empty");
            std::set<std::string> owners;
            const auto& shared = typed.transformations.front().transform;
            for (const auto& transformation : typed.transformations) {
                if (!(transformation.transform == shared))
                    document_error(DocumentErrorCode::invalid_entity, "Boundary transform group requires one shared transform");
                if (!owners.insert(transformation.boundary_id).second)
                    document_error(DocumentErrorCode::duplicate_change, "Boundary is transformed more than once");
                encoded["transformations"].push_back(command_to_json(
                    TransformBoundary{typed.expected_revision, transformation}).at("transformation"));
            }
            if (encoded.dump().size() > 1024 * 1024)
                document_error(DocumentErrorCode::invalid_entity, "Boundary transform group exceeds the persisted proof budget");
            return encoded;
        } else if constexpr (std::is_same_v<T, TranslateBoundaries>) {
            auto encoded = command_to_json(ApplyEntityChanges{
                typed.expected_revision, typed.entity_changes, {}, typed.message});
            encoded["kind"] = "translate_boundaries";
            encoded.erase("asset_changes");
            encoded["translations"] = nlohmann::json::array();
            if (typed.translations.empty())
                document_error(DocumentErrorCode::invalid_entity, "Boundary translation group is empty");
            std::unordered_set<std::string> owners;
            for (const auto& translation : typed.translations) {
                if (!owners.insert(translation.boundary_id).second)
                    document_error(DocumentErrorCode::duplicate_change, "Boundary is translated more than once");
                const auto single = command_to_json(TranslateBoundary{typed.expected_revision, translation});
                encoded["translations"].push_back(single.at("translation"));
            }
            return encoded;
        } else if constexpr (std::is_same_v<T, ApplyBoundaryConstraintChanges>) {
            try { validate_dimension_placement_intent(typed, false); }
            catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
            if(typed.wall_split) {
                if(!typed.boundary_edits.empty() || !typed.entity_changes.empty() || !typed.wall_edits.empty() ||
                    !typed.physical_entity_changes.empty() || !typed.exterior_source_edits.empty() ||
                    !typed.supplemental_entity_changes.empty() || !typed.supplemental_asset_changes.empty() ||
                    typed.exterior_source_completion || typed.supplemental_source_completion || typed.exterior_corner_move ||
                    typed.supplemental_asset_reference_completion || typed.rigid_wall_transform_completion ||
                    !typed.measured_stroke_edits.empty() || typed.measured_source_completion || typed.exterior_segment_resize || typed.exterior_segment_arc)
                    document_error(DocumentErrorCode::invalid_entity,"Wall split intent cannot borrow another command lane");
                return nlohmann::json{{"version",12},{"kind","apply_boundary_constraint_changes"},
                    {"expected_revision",typed.expected_revision},{"message",typed.message},{"wall_split",encode_wall_split(*typed.wall_split)}};
            }
            auto encoded = command_to_json(ApplyEntityChanges{
                typed.expected_revision, typed.entity_changes, {}, typed.message});
            encoded["kind"] = "apply_boundary_constraint_changes";
            encoded.erase("asset_changes");
            encoded["boundary_edits"] = nlohmann::json::array();
            try {
                for (const auto& edit : typed.boundary_edits)
                    encoded["boundary_edits"].push_back(encode_boundary_geometry_edit(edit));
                if (!typed.wall_edits.empty()) {
                    encoded["version"] = std::any_of(typed.wall_edits.begin(),typed.wall_edits.end(),
                        [](const auto& edit) { return edit.version==3; }) ? 5 :
                        std::any_of(typed.wall_edits.begin(),typed.wall_edits.end(),
                        [](const auto& edit) { return edit.version==2; }) ? 3 :
                        typed.boundary_edits.empty() ? 4 : 2;
                    encoded["wall_edits"] = nlohmann::json::array();
                    for (const auto& edit : typed.wall_edits)
                        encoded["wall_edits"].push_back(encode_constraint_wall_edit(edit));
                }
                if (has_exterior_source_completion(typed)) {
                    encoded["version"] = 6;
                    if (!encoded.contains("wall_edits")) encoded["wall_edits"] = nlohmann::json::array();
                    encoded["physical_entity_changes"] = command_to_json(ApplyEntityChanges{
                        typed.expected_revision, typed.physical_entity_changes, {}, typed.message}).at("entity_changes");
                    encoded["exterior_source_edits"] = nlohmann::json::array();
                    if (typed.exterior_source_edits.empty())
                        throw std::invalid_argument("Exterior source completion requires explicit redraws");
                    for (const auto& edit : typed.exterior_source_edits)
                    {
                        validate_exterior_source_redraw(edit);
                        encoded["exterior_source_edits"].push_back(encode_boundary_geometry_edit(edit));
                    }
                    if (has_supplemental_source_completion(typed)) {
                        encoded["version"] = typed.supplemental_asset_reference_completion ? 9 : 7;
                        const auto supplements = command_to_json(ApplyEntityChanges{
                            typed.expected_revision, typed.supplemental_entity_changes,
                            typed.supplemental_asset_reference_completion ? std::vector<AssetChange>{} :
                                typed.supplemental_asset_changes, typed.message});
                        encoded["supplemental_entity_changes"] = supplements.at("entity_changes");
                        encoded["supplemental_asset_changes"] = typed.supplemental_asset_reference_completion ?
                            command_asset_references_to_json(typed.supplemental_asset_changes) : supplements.at("asset_changes");
                    }
                    if (typed.exterior_corner_move) {
                        if (has_supplemental_source_completion(typed) || !typed.physical_entity_changes.empty())
                            throw std::invalid_argument("Exterior corner proof cannot carry ordinary physical or asset supplements");
                        encoded["version"] = 8;
                        encoded["exterior_corner_move"] = encode_exterior_corner_move(*typed.exterior_corner_move);
                    }
                    if (encoded.dump().size() > 1024 * 1024)
                        throw std::invalid_argument("Exterior source completion exceeds the persisted proof budget");
                }
                if(has_rigid_wall_transform(typed)) {
                    if(typed.exterior_corner_move)throw std::invalid_argument("Rigid wall envelope cannot borrow exterior corner authority");
                    encoded["version"]=10;
                    if(!encoded.contains("wall_edits"))encoded["wall_edits"]=nlohmann::json::array();
                    encoded["source_completion"]=has_exterior_source_completion(typed);
                    encoded["supplemental_source_completion"]=has_supplemental_source_completion(typed);
                    encoded["supplemental_asset_reference_completion"]=typed.supplemental_asset_reference_completion;
                    encoded["physical_entity_changes"]=command_to_json(ApplyEntityChanges{
                        typed.expected_revision,typed.physical_entity_changes,{},typed.message}).at("entity_changes");
                    if(!encoded.contains("exterior_source_edits"))encoded["exterior_source_edits"]=nlohmann::json::array();
                    const auto supplements=command_to_json(ApplyEntityChanges{typed.expected_revision,typed.supplemental_entity_changes,
                        typed.supplemental_asset_reference_completion ? std::vector<AssetChange>{} : typed.supplemental_asset_changes,typed.message});
                    encoded["supplemental_entity_changes"]=supplements.at("entity_changes");
                    encoded["supplemental_asset_changes"]=typed.supplemental_asset_reference_completion ?
                        command_asset_references_to_json(typed.supplemental_asset_changes) : supplements.at("asset_changes");
                    if(encoded.dump().size()>1024*1024)throw std::invalid_argument("Rigid wall proof exceeds the persisted proof budget");
                }
                if(has_measured_source_completion(typed)) {
                    encoded["version"]=11;
                    if(!encoded.contains("wall_edits"))encoded["wall_edits"]=nlohmann::json::array();
                    encoded["source_completion"]=has_exterior_source_completion(typed);
                    encoded["supplemental_source_completion"]=has_supplemental_source_completion(typed);
                    encoded["supplemental_asset_reference_completion"]=typed.supplemental_asset_reference_completion;
                    encoded["rigid_wall_transform_completion"]=has_rigid_wall_transform(typed);
                    encoded["measured_source_completion"]=true;
                    encoded["measured_stroke_edits"]=nlohmann::json::array();
                    std::set<std::string,std::less<>> owners;
                    for(const auto& edit:typed.measured_stroke_edits) {
                        if(!owners.insert(edit.stroke_id).second)throw std::invalid_argument("Measured stroke proof repeats an owner");
                        encoded["measured_stroke_edits"].push_back(command_measured_stroke_edit_to_json(edit));
                    }
                    encoded["physical_entity_changes"]=command_to_json(ApplyEntityChanges{
                        typed.expected_revision,typed.physical_entity_changes,{},typed.message}).at("entity_changes");
                    if(!encoded.contains("exterior_source_edits"))encoded["exterior_source_edits"]=nlohmann::json::array();
                    const auto supplements=command_to_json(ApplyEntityChanges{typed.expected_revision,typed.supplemental_entity_changes,
                        typed.supplemental_asset_reference_completion ? std::vector<AssetChange>{} : typed.supplemental_asset_changes,typed.message});
                    encoded["supplemental_entity_changes"]=supplements.at("entity_changes");
                    encoded["supplemental_asset_changes"]=typed.supplemental_asset_reference_completion ?
                        command_asset_references_to_json(typed.supplemental_asset_changes) : supplements.at("asset_changes");
                    encoded["exterior_corner_move"]=typed.exterior_corner_move ? encode_exterior_corner_move(*typed.exterior_corner_move) : nlohmann::json(nullptr);
                    if(encoded.dump().size()>1024*1024)throw std::invalid_argument("Measured constraint proof exceeds the persisted proof budget");
                }
                // Select the new dialect last: a measured-stroke completion may
                // accompany a source resize without replacing its authority.
                if (typed.exterior_segment_resize) {
                    validate_exterior_resize_related_edits(typed);
                    if (typed.exterior_corner_move || typed.exterior_segment_arc || has_rigid_wall_transform(typed) ||
                        has_supplemental_source_completion(typed) || typed.supplemental_asset_reference_completion ||
                        !typed.physical_entity_changes.empty())
                        throw std::invalid_argument("Exterior segment resize cannot borrow another physical or supplemental intent");
                    encoded["version"] = 13;
                    encoded["source_completion"] = true;
                    encoded["supplemental_source_completion"] = false;
                    encoded["supplemental_asset_reference_completion"] = false;
                    encoded["rigid_wall_transform_completion"] = false;
                    encoded["measured_source_completion"] = has_measured_source_completion(typed);
                    if (!encoded.contains("measured_stroke_edits")) encoded["measured_stroke_edits"] = nlohmann::json::array();
                    encoded["supplemental_entity_changes"] = nlohmann::json::array();
                    encoded["supplemental_asset_changes"] = nlohmann::json::array();
                    encoded.erase("exterior_corner_move");
                    encoded["exterior_segment_resize"] = encode_exterior_segment_resize(*typed.exterior_segment_resize);
                    if(encoded.dump().size()>1024*1024)throw std::invalid_argument("Exterior segment resize proof exceeds the persisted proof budget");
                }
                // Curvature authority must survive late measured source completion.
                if (typed.exterior_segment_arc) {
                    validate_exterior_resize_related_edits(typed);
                    if (typed.exterior_corner_move || typed.exterior_segment_resize || has_rigid_wall_transform(typed) ||
                        has_supplemental_source_completion(typed) || typed.supplemental_asset_reference_completion ||
                        !typed.physical_entity_changes.empty())
                        throw std::invalid_argument("Exterior segment arc cannot borrow another physical or supplemental intent");
                    encoded["version"] = 14;
                    encoded["source_completion"] = true;
                    encoded["supplemental_source_completion"] = false;
                    encoded["supplemental_asset_reference_completion"] = false;
                    encoded["rigid_wall_transform_completion"] = false;
                    encoded["measured_source_completion"] = has_measured_source_completion(typed);
                    if (!encoded.contains("measured_stroke_edits")) encoded["measured_stroke_edits"] = nlohmann::json::array();
                    encoded["supplemental_entity_changes"] = nlohmann::json::array();
                    encoded["supplemental_asset_changes"] = nlohmann::json::array();
                    encoded.erase("exterior_corner_move");
                    encoded["exterior_segment_arc"] = encode_exterior_segment_arc(*typed.exterior_segment_arc);
                    if(encoded.dump().size()>1024*1024)throw std::invalid_argument("Exterior segment arc proof exceeds the persisted proof budget");
                }
                if (has_dimension_placement_completion(typed)) {
                    encoded["version"] = 15;
                    if (!encoded.contains("wall_edits")) encoded["wall_edits"] = nlohmann::json::array();
                    encoded["source_completion"] = has_exterior_source_completion(typed);
                    encoded["supplemental_source_completion"] = has_supplemental_source_completion(typed);
                    encoded["supplemental_asset_reference_completion"] = typed.supplemental_asset_reference_completion;
                    encoded["rigid_wall_transform_completion"] = has_rigid_wall_transform(typed);
                    encoded["measured_source_completion"] = has_measured_source_completion(typed);
                    if (!encoded.contains("measured_stroke_edits")) encoded["measured_stroke_edits"] = nlohmann::json::array();
                    encoded["physical_entity_changes"] = command_to_json(ApplyEntityChanges{
                        typed.expected_revision, typed.physical_entity_changes, {}, typed.message}).at("entity_changes");
                    if (!encoded.contains("exterior_source_edits")) encoded["exterior_source_edits"] = nlohmann::json::array();
                    const auto supplements = command_to_json(ApplyEntityChanges{typed.expected_revision, typed.supplemental_entity_changes,
                        typed.supplemental_asset_reference_completion ? std::vector<AssetChange>{} : typed.supplemental_asset_changes, typed.message});
                    encoded["supplemental_entity_changes"] = supplements.at("entity_changes");
                    encoded["supplemental_asset_changes"] = typed.supplemental_asset_reference_completion ?
                        command_asset_references_to_json(typed.supplemental_asset_changes) : supplements.at("asset_changes");
                    encoded["exterior_corner_move"] = nullptr;
                    encoded["dimension_placement_completion"] = true;
                    encoded["dimension_placement_moves"] = nlohmann::json::array();
                    for (const auto& move : typed.dimension_placement_moves)
                        encoded["dimension_placement_moves"].push_back({{"dimension_id", move.dimension_id},
                            {"offset", {move.offset.x, move.offset.y}}});
                    if (encoded.dump().size() > 1024*1024)
                        throw std::invalid_argument("Dimension placement proof exceeds the persisted proof budget");
                }
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, error.what());
            }
            return encoded;
        } else if constexpr (std::is_same_v<T, NameRevision>) {
            validate_revision_name(typed.name);
            return nlohmann::json{{"version", 1}, {"kind", "name_revision"},
                                  {"expected_revision", typed.expected_revision}, {"name", typed.name}};
        } else if constexpr (std::is_same_v<T, TranslateBoundary>) {
            if (!is_valid_identifier(typed.translation.boundary_id))
                document_error(DocumentErrorCode::invalid_entity, "serialized boundary ID is invalid");
            (void)command_number(typed.translation.offset.x, "serialized translation offset.x");
            (void)command_number(typed.translation.offset.y, "serialized translation offset.y");
            return nlohmann::json{{"version", 1}, {"kind", "translate_boundary"},
                                  {"expected_revision", typed.expected_revision},
                                  {"translation", {{"boundary_id", typed.translation.boundary_id},
                                      {"offset", command_vec2_to_json(typed.translation.offset)}}}};
        } else if constexpr (std::is_same_v<T, TransformBoundary>) {
            if (!is_valid_identifier(typed.transformation.boundary_id))
                document_error(DocumentErrorCode::invalid_entity, "serialized boundary ID is invalid");
            (void)command_transform_from_json(command_transform_to_json(typed.transformation.transform));
            return nlohmann::json{{"version", 1}, {"kind", "transform_boundary"},
                                  {"expected_revision", typed.expected_revision},
                                  {"transformation", {{"boundary_id", typed.transformation.boundary_id},
                                      {"transform", command_transform_to_json(typed.transformation.transform)}}}};
        } else {
            try {
                return nlohmann::json{{"version", 1}, {"kind", "edit_boundary_geometry"},
                                      {"expected_revision", typed.expected_revision},
                                      {"edit", encode_boundary_geometry_edit(typed.edit)}};
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, error.what());
            }
        }
    }, command);
}

Command command_from_json(const nlohmann::json& value,
    const std::function<const Asset*(std::string_view)>& asset_resolver) {
    try {
        if (!value.is_object() || !value.contains("version") || !value.contains("kind") ||
            !value.at("version").is_number_integer() ||
            (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4 && value.at("version") != 5 && value.at("version") != 6 && value.at("version") != 7 && value.at("version") != 8 && value.at("version") != 9 && value.at("version") != 10 && value.at("version") != 11 && value.at("version") != 12 && value.at("version") != 13 && value.at("version") != 14 && value.at("version") != 15) ||
            !value.at("kind").is_string()) {
            document_error(DocumentErrorCode::invalid_entity, "serialized command envelope is invalid");
        }
        const auto kind = value.at("kind").get<std::string>();
        if (value.at("version") != 1 && kind != "apply_boundary_constraint_changes")
            document_error(DocumentErrorCode::invalid_entity,"Unsupported command envelope version");
        if (kind == "transform_boundaries") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "message", "entity_changes", "transformations"},
                                 DocumentErrorCode::invalid_entity, "serialized transform group");
            if (!value.at("transformations").is_array() || value.at("transformations").empty())
                document_error(DocumentErrorCode::invalid_entity, "Transformations must be a nonempty array");
            auto ordinary = value;
            ordinary["kind"] = "apply_entity_changes";
            ordinary.erase("transformations");
            ordinary["asset_changes"] = nlohmann::json::array();
            const auto changes = std::get<ApplyEntityChanges>(command_from_json(ordinary));
            TransformBoundaries result{changes.expected_revision, {}, changes.entity_changes, changes.message};
            std::set<std::string> owners;
            for (const auto& transformation : value.at("transformations")) {
                const auto single = std::get<TransformBoundary>(command_from_json(nlohmann::json{
                    {"version", 1}, {"kind", "transform_boundary"},
                    {"expected_revision", result.expected_revision}, {"transformation", transformation}}));
                if (!owners.insert(single.transformation.boundary_id).second)
                    document_error(DocumentErrorCode::duplicate_change, "Boundary is transformed more than once");
                if (!result.transformations.empty() &&
                    !(result.transformations.front().transform == single.transformation.transform))
                    document_error(DocumentErrorCode::invalid_entity, "Boundary transform group requires one shared transform");
                result.transformations.push_back(single.transformation);
            }
            return result;
        }
        if (kind == "translate_boundaries") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "message",
                                         "entity_changes", "translations"},
                                 DocumentErrorCode::invalid_entity, "serialized translation group");
            if (!value.at("translations").is_array() || value.at("translations").empty())
                document_error(DocumentErrorCode::invalid_entity, "Translations must be a nonempty array");
            auto ordinary = value;
            ordinary["kind"] = "apply_entity_changes";
            ordinary.erase("translations");
            ordinary["asset_changes"] = nlohmann::json::array();
            const auto changes = std::get<ApplyEntityChanges>(command_from_json(ordinary));
            TranslateBoundaries result{changes.expected_revision, {}, changes.entity_changes, changes.message};
            std::unordered_set<std::string> owners;
            for (const auto& translation : value.at("translations")) {
                const auto single = std::get<TranslateBoundary>(command_from_json(nlohmann::json{
                    {"version", 1}, {"kind", "translate_boundary"},
                    {"expected_revision", result.expected_revision}, {"translation", translation}}));
                if (!owners.insert(single.translation.boundary_id).second)
                    document_error(DocumentErrorCode::duplicate_change, "Boundary is translated more than once");
                result.translations.push_back(single.translation);
            }
            return result;
        }
        if (kind == "apply_boundary_constraint_changes") {
            if (value.at("version") == 15) {
                command_exact_fields(value,{"version","kind","expected_revision","message","entity_changes","boundary_edits","wall_edits",
                    "physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes",
                    "source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                    "rigid_wall_transform_completion","measured_source_completion","measured_stroke_edits","exterior_corner_move",
                    "dimension_placement_completion","dimension_placement_moves"},
                    DocumentErrorCode::invalid_entity,"serialized dimension placement command");
                if (!value.at("dimension_placement_completion").is_boolean() || !value.at("dimension_placement_completion").get<bool>() ||
                    !value.at("dimension_placement_moves").is_array() || !value.at("exterior_corner_move").is_null())
                    throw std::invalid_argument("Dimension placement mode disagrees with its typed lane");
                if (!value.at("measured_source_completion").is_boolean() || !value.at("measured_stroke_edits").is_array())
                    throw std::invalid_argument("Dimension placement measured mode is invalid");
                const bool measured = value.at("measured_source_completion").get<bool>();
                if (!measured && !value.at("measured_stroke_edits").empty())
                    throw std::invalid_argument("Dimension placement measured mode disagrees with its typed lane");
                // Delegate unchanged lanes to the strict historical decoder;
                // its measured discriminator permits an empty boundary lane.
                auto lanes = value; lanes["version"] = 11; lanes["measured_source_completion"] = true;
                lanes.erase("dimension_placement_completion"); lanes.erase("dimension_placement_moves");
                auto result = std::get<ApplyBoundaryConstraintChanges>(command_from_json(lanes, asset_resolver));
                result.measured_source_completion = measured;
                result.dimension_placement_completion = true;
                for (const auto& move : value.at("dimension_placement_moves")) {
                    command_exact_fields(move,{"dimension_id","offset"},DocumentErrorCode::invalid_entity,"serialized dimension placement move");
                    if (!move.at("dimension_id").is_string() || !move.at("offset").is_array() || move.at("offset").size()!=2 ||
                        !move.at("offset")[0].is_number() || !move.at("offset")[1].is_number())
                        throw std::invalid_argument("Dimension placement move is malformed");
                    result.dimension_placement_moves.push_back({move.at("dimension_id").get<std::string>(),
                        {move.at("offset")[0].get<double>(),move.at("offset")[1].get<double>()}});
                }
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 14) {
                command_exact_fields(value,{"version","kind","expected_revision","message","entity_changes","boundary_edits","wall_edits",
                    "physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes",
                    "source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                    "rigid_wall_transform_completion","measured_source_completion","measured_stroke_edits","exterior_segment_arc"},
                    DocumentErrorCode::invalid_entity,"serialized exterior segment arc command");
                for (const auto* flag : {"source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                                        "rigid_wall_transform_completion","measured_source_completion"})
                    if (!value.at(flag).is_boolean()) throw std::invalid_argument("Exterior arc completion modes must be booleans");
                if (!value.at("source_completion").get<bool>() || value.at("supplemental_source_completion").get<bool>() ||
                    value.at("supplemental_asset_reference_completion").get<bool>() || value.at("rigid_wall_transform_completion").get<bool>())
                    throw std::invalid_argument("Exterior arc has incompatible completion modes");
                for (const auto* lane : {"physical_entity_changes","supplemental_entity_changes","supplemental_asset_changes"})
                    if (!value.at(lane).is_array() || !value.at(lane).empty())
                        throw std::invalid_argument("Exterior arc cannot carry raw physical or supplemental changes");
                const bool measured = value.at("measured_source_completion").get<bool>();
                if (!value.at("measured_stroke_edits").is_array() || (!measured && !value.at("measured_stroke_edits").empty()))
                    throw std::invalid_argument("Exterior arc measured mode disagrees with its typed lane");
                // Existing strict decoders continue to own their unchanged lanes.
                auto lanes = value;
                lanes["version"] = 11;
                lanes["measured_source_completion"] = true;
                lanes.erase("exterior_segment_arc");
                lanes["exterior_corner_move"] = nullptr;
                auto result = std::get<ApplyBoundaryConstraintChanges>(command_from_json(lanes, asset_resolver));
                result.measured_source_completion = measured;
                result.exterior_segment_arc = decode_exterior_segment_arc(value.at("exterior_segment_arc"));
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 13) {
                command_exact_fields(value,{"version","kind","expected_revision","message","entity_changes","boundary_edits","wall_edits",
                    "physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes",
                    "source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                    "rigid_wall_transform_completion","measured_source_completion","measured_stroke_edits","exterior_segment_resize"},
                    DocumentErrorCode::invalid_entity,"serialized exterior segment resize command");
                for (const auto* flag : {"source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                                        "rigid_wall_transform_completion","measured_source_completion"})
                    if (!value.at(flag).is_boolean()) throw std::invalid_argument("Exterior resize completion modes must be booleans");
                if (!value.at("source_completion").get<bool>() || value.at("supplemental_source_completion").get<bool>() ||
                    value.at("supplemental_asset_reference_completion").get<bool>() || value.at("rigid_wall_transform_completion").get<bool>())
                    throw std::invalid_argument("Exterior resize has incompatible completion modes");
                for (const auto* lane : {"physical_entity_changes","supplemental_entity_changes","supplemental_asset_changes"})
                    if (!value.at(lane).is_array() || !value.at(lane).empty())
                        throw std::invalid_argument("Exterior resize cannot carry raw physical or supplemental changes");
                const bool measured = value.at("measured_source_completion").get<bool>();
                if (!value.at("measured_stroke_edits").is_array() || (!measured && !value.at("measured_stroke_edits").empty()))
                    throw std::invalid_argument("Exterior resize measured mode disagrees with its typed lane");
                // Reuse the strict existing lane decoders without changing any
                // older dialect's field set or replay behavior.
                auto lanes = value;
                lanes["version"] = 11;
                lanes["measured_source_completion"] = true;
                lanes.erase("exterior_segment_resize");
                lanes["exterior_corner_move"] = nullptr;
                auto result = std::get<ApplyBoundaryConstraintChanges>(command_from_json(lanes, asset_resolver));
                result.measured_source_completion = measured;
                result.exterior_segment_resize = decode_exterior_segment_resize(value.at("exterior_segment_resize"));
                (void)command_to_json(Command{result});
                return result;
            }
            if(value.at("version")==12) {
                command_exact_fields(value,{"version","kind","expected_revision","message","wall_split"},
                    DocumentErrorCode::invalid_entity,"serialized wall split command");
                if(!value.at("message").is_string())document_error(DocumentErrorCode::invalid_entity,"Wall split message must be a string");
                ApplyBoundaryConstraintChanges result;result.expected_revision=command_revision(value.at("expected_revision"),"Wall split revision");
                result.message=value.at("message").get<std::string>();result.wall_split=decode_wall_split(value.at("wall_split"));
                return result;
            }
            const bool measured_envelope=value.at("version")==11;
            const bool combined_envelope=value.at("version")==10 || measured_envelope;
            if(measured_envelope) {
                command_exact_fields(value,{"version","kind","expected_revision","message","entity_changes","boundary_edits","wall_edits",
                    "physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes",
                    "source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                    "rigid_wall_transform_completion","measured_source_completion","measured_stroke_edits","exterior_corner_move"},
                    DocumentErrorCode::invalid_entity,"serialized measured constraint command");
                for(const auto* flag:{"rigid_wall_transform_completion","measured_source_completion"})
                    if(!value.at(flag).is_boolean())document_error(DocumentErrorCode::invalid_entity,"Measured completion modes must be booleans");
                if(!value.at("measured_source_completion").get<bool>() || !value.at("measured_stroke_edits").is_array())
                    document_error(DocumentErrorCode::invalid_entity,"Envelope eleven requires measured source completion");
            }
            const bool rigid_envelope=value.at("version")==10 ||
                (measured_envelope && value.at("rigid_wall_transform_completion").get<bool>());
            if(combined_envelope) {
                if(!measured_envelope)
                command_exact_fields(value,{"version","kind","expected_revision","message","entity_changes","boundary_edits","wall_edits",
                    "physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes",
                    "source_completion","supplemental_source_completion","supplemental_asset_reference_completion"},
                    DocumentErrorCode::invalid_entity,"serialized rigid wall command");
                for(const auto* flag:{"source_completion","supplemental_source_completion","supplemental_asset_reference_completion"})
                    if(!value.at(flag).is_boolean())document_error(DocumentErrorCode::invalid_entity,"Rigid wall completion modes must be booleans");
                if(value.dump().size()>1024*1024)document_error(DocumentErrorCode::invalid_entity,"Rigid wall proof exceeds the persisted proof budget");
            }
            const bool mixed = value.at("version") != 1;
            const bool asset_references = combined_envelope ? value.at("supplemental_asset_reference_completion").get<bool>() : value.at("version") == 9;
            const bool supplements = combined_envelope ? value.at("supplemental_source_completion").get<bool>() : value.at("version") == 7 || asset_references;
            const bool corner_move = value.at("version") == 8 || (measured_envelope && !value.at("exterior_corner_move").is_null());
            const bool source_completion = combined_envelope ? value.at("source_completion").get<bool>() : value.at("version") == 6 || supplements || corner_move;
            if(combined_envelope) {
                for(const auto* lane:{"physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes"})
                    if(!value.at(lane).is_array())document_error(DocumentErrorCode::invalid_entity,"Rigid wall command lanes must be arrays");
                if((!source_completion && (!value.at("physical_entity_changes").empty() || !value.at("exterior_source_edits").empty())) ||
                    (!supplements && (asset_references || !value.at("supplemental_entity_changes").empty() || !value.at("supplemental_asset_changes").empty())))
                    document_error(DocumentErrorCode::invalid_entity,"Rigid wall completion modes disagree with retained lanes");
                if(corner_move && (!source_completion || supplements || rigid_envelope))
                    document_error(DocumentErrorCode::invalid_entity,"Exterior corner mode conflicts with other completion modes");
            }
            else if (corner_move) command_exact_fields(value, {"version","kind","expected_revision","message",
                "entity_changes","boundary_edits","wall_edits","physical_entity_changes","exterior_source_edits","exterior_corner_move"},
                DocumentErrorCode::invalid_entity,"serialized exterior corner command");
            else if (supplements) {
                command_exact_fields(value, {"version","kind","expected_revision","message",
                    "entity_changes","boundary_edits","wall_edits","physical_entity_changes","exterior_source_edits",
                    "supplemental_entity_changes","supplemental_asset_changes"},
                    DocumentErrorCode::invalid_entity,"serialized supplemental exterior source command");
                if (value.dump().size() > 1024 * 1024)
                    document_error(DocumentErrorCode::invalid_entity,
                        "Exterior source completion exceeds the persisted proof budget");
            } else if (source_completion) command_exact_fields(value, {"version","kind","expected_revision","message",
                                          "entity_changes","boundary_edits","wall_edits",
                                          "physical_entity_changes","exterior_source_edits"},
                                 DocumentErrorCode::invalid_entity,"serialized exterior source command");
            else if (mixed) command_exact_fields(value, {"version","kind","expected_revision","message",
                                          "entity_changes","boundary_edits","wall_edits"},
                                 DocumentErrorCode::invalid_entity,"serialized mixed constraint command");
            else command_exact_fields(value, {"version", "kind", "expected_revision", "message",
                                          "entity_changes", "boundary_edits"},
                                 DocumentErrorCode::invalid_entity, "serialized boundary constraint command");
            if (!value.at("boundary_edits").is_array() ||
                (value.at("boundary_edits").empty() && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5 && !source_completion && !rigid_envelope && !measured_envelope))
                document_error(DocumentErrorCode::invalid_entity, "Boundary edits must be a nonempty array");
            if (value.at("version")==4 && !value.at("boundary_edits").empty())
                document_error(DocumentErrorCode::invalid_entity, "Version 4 requires a straight wall-only transaction");
            auto ordinary = value;
            ordinary["kind"] = "apply_entity_changes";
            ordinary["version"] = 1;
            ordinary.erase("wall_edits");
            ordinary.erase("boundary_edits");
            ordinary.erase("physical_entity_changes");
            ordinary.erase("exterior_source_edits");
            ordinary.erase("supplemental_entity_changes");
            ordinary.erase("supplemental_asset_changes");
            ordinary.erase("exterior_corner_move");
            ordinary.erase("source_completion");
            ordinary.erase("supplemental_source_completion");
            ordinary.erase("supplemental_asset_reference_completion");
            ordinary.erase("rigid_wall_transform_completion");
            ordinary.erase("measured_source_completion");
            ordinary.erase("measured_stroke_edits");
            ordinary["asset_changes"] = nlohmann::json::array();
            const auto changes = std::get<ApplyEntityChanges>(command_from_json(ordinary));
            ApplyBoundaryConstraintChanges result{
                changes.expected_revision, {}, changes.entity_changes, changes.message};
            result.exterior_source_completion = source_completion;
            result.supplemental_source_completion = supplements;
            result.supplemental_asset_reference_completion = asset_references;
            result.rigid_wall_transform_completion=rigid_envelope;
            result.measured_source_completion=measured_envelope;
            try {
                if(measured_envelope) {
                    std::set<std::string,std::less<>> owners;
                    for(const auto& edit:value.at("measured_stroke_edits")) {
                        auto decoded=command_measured_stroke_edit_from_json(edit);
                        if(!owners.insert(decoded.stroke_id).second)throw std::invalid_argument("Measured stroke proof repeats an owner");
                        result.measured_stroke_edits.push_back(std::move(decoded));
                    }
                }
                if (corner_move) {
                    if (value.dump().size() > 1024 * 1024) throw std::invalid_argument("Exterior corner proof exceeds the persisted proof budget");
                    result.exterior_corner_move = decode_exterior_corner_move(value.at("exterior_corner_move"));
                    if (!value.at("physical_entity_changes").is_array() || !value.at("physical_entity_changes").empty())
                        throw std::invalid_argument("Exterior corner proof cannot contain raw physical wall changes");
                }
                for (const auto& edit : value.at("boundary_edits"))
                    result.boundary_edits.push_back(decode_boundary_geometry_edit(edit));
                if (mixed) {
                    if (!value.at("wall_edits").is_array() || (value.at("wall_edits").empty() && !source_completion && !rigid_envelope && !measured_envelope))
                        document_error(DocumentErrorCode::invalid_entity,"Versioned wall transaction requires nonempty wall edits");
                    for (const auto& edit : value.at("wall_edits"))
                        result.wall_edits.push_back(decode_constraint_wall_edit(edit));
                    if(!rigid_envelope && std::any_of(result.wall_edits.begin(),result.wall_edits.end(),[](const auto& edit){return edit.version==4;}))
                        document_error(DocumentErrorCode::invalid_entity,"Rigid wall proof requires command envelope 10");
                    const bool physical_curve=std::any_of(result.wall_edits.begin(),result.wall_edits.end(),
                        [](const auto& edit) { return edit.version==3; });
                    const bool curved=std::any_of(result.wall_edits.begin(),result.wall_edits.end(),
                        [](const auto& edit) { return edit.version==2; });
                    if (!source_completion && !rigid_envelope && !measured_envelope && physical_curve!=(value.at("version")==5))
                        document_error(DocumentErrorCode::invalid_entity,"Physical curve-length proof requires exactly command version 5");
                    if (!source_completion && !rigid_envelope && !measured_envelope && !physical_curve && curved!=(value.at("version")==3))
                        document_error(DocumentErrorCode::invalid_entity,"Curved wall proof requires exactly command version 3");
                }
                if (source_completion) {
                    auto physical = ordinary;
                    physical["entity_changes"] = value.at("physical_entity_changes");
                    result.physical_entity_changes = std::get<ApplyEntityChanges>(command_from_json(physical)).entity_changes;
                    if (!value.at("exterior_source_edits").is_array() || value.at("exterior_source_edits").empty())
                        document_error(DocumentErrorCode::invalid_entity,"Exterior source completion requires explicit redraws");
                    for (const auto& edit : value.at("exterior_source_edits")) {
                        if (!edit.is_object() || !edit.contains("version") || edit.at("version") != 3)
                            throw std::invalid_argument("Automatic exterior source updates require version-three redraws");
                        const auto decoded = decode_boundary_geometry_edit(edit);
                        validate_exterior_source_redraw(decoded);
                        result.exterior_source_edits.push_back(decoded);
                    }
                }
                if (supplements) {
                    auto supplemental = ordinary;
                    supplemental["entity_changes"] = value.at("supplemental_entity_changes");
                    supplemental["asset_changes"] = asset_references ? nlohmann::json::array() : value.at("supplemental_asset_changes");
                    const auto decoded = std::get<ApplyEntityChanges>(command_from_json(supplemental));
                    result.supplemental_entity_changes = decoded.entity_changes;
                    result.supplemental_asset_changes = asset_references ?
                        command_asset_references_from_json(value.at("supplemental_asset_changes"), asset_resolver) : decoded.asset_changes;
                }
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, error.what());
            }
            return result;
        }
        if (kind == "apply_entity_changes") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "message",
                                          "entity_changes", "asset_changes"},
                                 DocumentErrorCode::invalid_entity, "serialized apply command");
            if (!value.at("entity_changes").is_array() || !value.at("asset_changes").is_array())
                document_error(DocumentErrorCode::invalid_entity, "serialized change lists must be arrays");
            ApplyEntityChanges result;
            result.expected_revision = command_revision(value.at("expected_revision"), "command expected_revision");
            if (!value.at("message").is_string() || value.at("message").get<std::string>().size() > 1024 ||
                !is_valid_utf8_without_nul(value.at("message").get<std::string>()))
                document_error(DocumentErrorCode::invalid_entity, "serialized command message is invalid");
            result.message = value.at("message").get<std::string>();
            for (const auto& encoded : value.at("entity_changes")) {
                if (!encoded.is_object() || !encoded.contains("kind") || !encoded.at("kind").is_string())
                    document_error(DocumentErrorCode::invalid_entity, "serialized entity change is invalid");
                const auto change_kind = encoded.at("kind").get<std::string>();
                if (change_kind == "upsert") {
                    command_exact_fields(encoded, {"kind", "entity"}, DocumentErrorCode::invalid_entity,
                                         "serialized entity upsert");
                    result.entity_changes.push_back(EntityChange::upsert(command_entity_from_json(encoded.at("entity"))));
                } else if (change_kind == "erase") {
                    command_exact_fields(encoded, {"kind", "entity_id"}, DocumentErrorCode::invalid_entity,
                                         "serialized entity erase");
                    result.entity_changes.push_back(EntityChange::erase(command_string(encoded.at("entity_id"),
                        "serialized entity erase ID", kMaximumIdBytes)));
                } else {
                    document_error(DocumentErrorCode::invalid_entity, "unknown serialized entity change kind");
                }
            }
            for (const auto& encoded : value.at("asset_changes")) {
                if (!encoded.is_object() || !encoded.contains("kind") || !encoded.at("kind").is_string())
                    document_error(DocumentErrorCode::invalid_asset, "serialized asset change is invalid");
                const auto change_kind = encoded.at("kind").get<std::string>();
                if (change_kind == "upsert") {
                    command_exact_fields(encoded, {"kind", "asset"}, DocumentErrorCode::invalid_asset,
                                         "serialized asset upsert");
                    result.asset_changes.push_back(AssetChange::upsert(command_asset_from_json(encoded.at("asset"))));
                } else if (change_kind == "erase") {
                    command_exact_fields(encoded, {"kind", "asset_id"}, DocumentErrorCode::invalid_asset,
                                         "serialized asset erase");
                    result.asset_changes.push_back(AssetChange::erase(command_string(encoded.at("asset_id"),
                        "serialized asset erase ID", kMaximumIdBytes)));
                } else {
                    document_error(DocumentErrorCode::invalid_asset, "unknown serialized asset change kind");
                }
            }
            return result;
        }
        if (kind == "name_revision") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "name"},
                                 DocumentErrorCode::invalid_entity, "serialized name command");
            NameRevision result{command_revision(value.at("expected_revision"), "command expected_revision"),
                                command_string(value.at("name"), "serialized revision name", 256)};
            validate_revision_name(result.name);
            return result;
        }
        if (kind == "translate_boundary") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "translation"},
                                 DocumentErrorCode::invalid_entity, "serialized translation command");
            const auto& translation = value.at("translation");
            command_exact_fields(translation, {"boundary_id", "offset"}, DocumentErrorCode::invalid_entity,
                                 "serialized translation");
            TranslateBoundary result;
            result.expected_revision = command_revision(value.at("expected_revision"), "command expected_revision");
            result.translation.boundary_id = command_string(translation.at("boundary_id"),
                "serialized boundary ID", kMaximumIdBytes);
            if (!is_valid_identifier(result.translation.boundary_id))
                document_error(DocumentErrorCode::invalid_entity, "serialized boundary ID is invalid");
            result.translation.offset = command_vec2_from_json(translation.at("offset"), "serialized translation offset");
            return result;
        }
        if (kind == "transform_boundary") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "transformation"},
                                 DocumentErrorCode::invalid_entity, "serialized transform command");
            const auto& transformation = value.at("transformation");
            command_exact_fields(transformation, {"boundary_id", "transform"}, DocumentErrorCode::invalid_entity,
                                 "serialized transformation");
            TransformBoundary result;
            result.expected_revision = command_revision(value.at("expected_revision"), "command expected_revision");
            result.transformation.boundary_id = command_string(transformation.at("boundary_id"),
                "serialized boundary ID", kMaximumIdBytes);
            if (!is_valid_identifier(result.transformation.boundary_id))
                document_error(DocumentErrorCode::invalid_entity, "serialized boundary ID is invalid");
            result.transformation.transform = command_transform_from_json(transformation.at("transform"));
            return result;
        }
        if (kind == "edit_boundary_geometry") {
            command_exact_fields(value, {"version", "kind", "expected_revision", "edit"},
                                 DocumentErrorCode::invalid_entity,
                                 "serialized boundary geometry edit command");
            EditBoundaryGeometry result;
            result.expected_revision = command_revision(
                value.at("expected_revision"), "command expected_revision");
            try {
                result.edit = decode_boundary_geometry_edit(value.at("edit"));
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, error.what());
            }
            return result;
        }
        document_error(DocumentErrorCode::invalid_entity, "unknown serialized command kind");
    } catch (const DocumentError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        document_error(DocumentErrorCode::invalid_entity,
                       std::string("serialized command JSON is invalid: ") + error.what());
    }
}

EntityChange EntityChange::upsert(Entity entity) {
    const auto id = entity.id;
    return EntityChange{EntityChangeKind::upsert, std::move(entity), id};
}

EntityChange EntityChange::erase(std::string entity_id) {
    return EntityChange{EntityChangeKind::erase, {}, std::move(entity_id)};
}

AssetChange AssetChange::upsert(Asset asset) {
    const auto id = asset.id;
    return AssetChange{AssetChangeKind::upsert, std::move(asset), id};
}

AssetChange AssetChange::erase(std::string asset_id) {
    return AssetChange{AssetChangeKind::erase, {}, std::move(asset_id)};
}

const std::string& DocumentSnapshot::document_id() const noexcept { return document_id_; }
Revision DocumentSnapshot::revision() const noexcept { return revision_; }
std::optional<Revision> DocumentSnapshot::saved_revision_optional() const noexcept {
    return saved_revision_;
}
Revision DocumentSnapshot::saved_revision() const noexcept { return saved_revision_.value_or(0); }
bool DocumentSnapshot::dirty() const noexcept {
    return !saved_revision_.has_value() || *saved_revision_ != revision_;
}
bool DocumentSnapshot::is_editable() const noexcept { return editable_; }
const std::string& DocumentSnapshot::read_only_reason() const noexcept { return read_only_reason_; }
const std::map<std::string, Entity, std::less<>>& DocumentSnapshot::entities() const noexcept {
    return history_.at(static_cast<std::size_t>(revision_)).entities;
}
const std::map<std::string, Asset, std::less<>>& DocumentSnapshot::assets() const noexcept {
    return history_.at(static_cast<std::size_t>(revision_)).assets;
}
const std::vector<RevisionRecord>& DocumentSnapshot::history() const noexcept { return history_; }
const std::map<std::string, Revision, std::less<>>& DocumentSnapshot::named_revisions() const noexcept {
    return named_revisions_;
}

Document::Document(std::string document_id) : document_id_(std::move(document_id)) {}

Document Document::create() { return create({}, {}); }

Document Document::create(std::vector<Entity> initial_entities, std::vector<Asset> initial_assets) {
    Document document(make_stable_id());
    RevisionRecord initial;
    initial.revision = 0;
    initial.action = "create";
    for (auto& entity : initial_entities) {
        validate_entity(entity);
        if (!initial.entities.emplace(entity.id, std::move(entity)).second) {
            document_error(DocumentErrorCode::duplicate_change, "duplicate initial entity id");
        }
    }
    for (auto& asset : initial_assets) {
        validate_asset(asset);
        if (!initial.assets.emplace(asset.id, std::move(asset)).second) {
            document_error(DocumentErrorCode::duplicate_change, "duplicate initial asset id");
        }
    }
    document.unsupported_constraint_history_reason_ = validate_state(initial.entities, initial.assets);
    record_boundary_identities(document.boundary_identity_history_, initial.entities);
    document.history_.push_back(std::move(initial));
    document.update_editability();
    return document;
}

Revision Document::revision() const noexcept { return head_revision_; }
std::optional<Revision> Document::saved_revision_optional() const noexcept { return saved_revision_; }
Revision Document::saved_revision() const noexcept { return saved_revision_.value_or(0); }
bool Document::dirty() const noexcept {
    return !saved_revision_.has_value() || *saved_revision_ != head_revision_;
}
bool Document::is_editable() const noexcept { return editable_; }
const std::string& Document::read_only_reason() const noexcept { return read_only_reason_; }
bool Document::can_undo() const noexcept { return !head_record().undo_stack.empty(); }
bool Document::can_redo() const noexcept { return !head_record().redo_stack.empty(); }

const RevisionRecord& Document::head_record() const {
    if (head_revision_ >= history_.size() || history_[static_cast<std::size_t>(head_revision_)].revision !=
                                                    head_revision_) {
        document_error(DocumentErrorCode::invalid_history, "document head is absent from history");
    }
    return history_[static_cast<std::size_t>(head_revision_)];
}

DocumentSnapshot Document::snapshot() const {
    DocumentSnapshot snapshot;
    snapshot.document_id_ = document_id_;
    snapshot.revision_ = head_revision_;
    snapshot.saved_revision_ = saved_revision_;
    snapshot.editable_ = editable_;
    snapshot.read_only_reason_ = read_only_reason_;
    snapshot.history_ = history_;
    snapshot.named_revisions_ = named_revisions_;
    return snapshot;
}

Document Document::fork(const DocumentSnapshot& source) {
    return restore(source);
}

Document Document::fork_at_revision(const DocumentSnapshot& source, Revision revision) {
    (void)fork(source);
    if (revision >= source.history_.size())
        document_error(DocumentErrorCode::invalid_history, "requested revision is not retained");
    auto prefix = source;
    prefix.history_.resize(static_cast<std::size_t>(revision) + 1);
    prefix.revision_ = revision;
    if (prefix.saved_revision_ && *prefix.saved_revision_ > revision) prefix.saved_revision_.reset();
    for (auto it = prefix.named_revisions_.begin(); it != prefix.named_revisions_.end();) {
        if (it->second > revision) it = prefix.named_revisions_.erase(it);
        else ++it;
    }
    return restore(std::move(prefix));
}

DocumentSnapshot Document::preview_command(const DocumentSnapshot& source, const Command& command) {
    auto candidate = fork(source);
    candidate.apply(command);
    return candidate.snapshot();
}

Revision Document::apply(const Command& command) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    return std::visit(
        [this](const auto& typed_command) -> Revision {
            validate_expected_revision(head_revision_, typed_command.expected_revision);
            const auto& current = head_record();
            RevisionRecord next;
            next.revision = static_cast<Revision>(history_.size());
            next.parent_revision = head_revision_;
            next.entities = current.entities;
            next.assets = current.assets;
            next.undo_stack = current.undo_stack;
            next.undo_stack.push_back(head_revision_);
            std::optional<std::string> next_unsupported_constraints;
            auto next_identity_history = boundary_identity_history_;

            using CommandType = std::decay_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<CommandType, ApplyEntityChanges>) {
                next.action = command_message(typed_command);
                validate_action(next.action);
                std::unordered_set<std::string> touched_entities;
                for (const auto& change : typed_command.entity_changes) {
                    const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id
                                                                            : change.entity_id;
                    if (!touched_entities.insert(id).second) {
                        document_error(DocumentErrorCode::duplicate_change,
                                       "entity is changed more than once in one command: " + id);
                    }
                    if (change.kind == EntityChangeKind::upsert) {
                        validate_entity(change.entity);
                        next.entities.insert_or_assign(change.entity.id, change.entity);
                    } else {
                        if (!is_valid_identifier(change.entity_id)) {
                            document_error(DocumentErrorCode::invalid_entity,
                                           "deleted entity id is invalid");
                        }
                        next.entities.erase(change.entity_id);
                    }
                }
                std::unordered_set<std::string> touched_assets;
                for (const auto& change : typed_command.asset_changes) {
                    const auto& id = change.kind == AssetChangeKind::upsert ? change.asset.id
                                                                           : change.asset_id;
                    if (!touched_assets.insert(id).second) {
                        document_error(DocumentErrorCode::duplicate_change,
                                       "asset is changed more than once in one command: " + id);
                    }
                    if (change.kind == AssetChangeKind::upsert) {
                        validate_asset(change.asset);
                        next.assets.insert_or_assign(change.asset.id, change.asset);
                    } else {
                        if (!is_valid_identifier(change.asset_id)) {
                            document_error(DocumentErrorCode::invalid_asset,
                                           "deleted asset id is invalid");
                        }
                        next.assets.erase(change.asset_id);
                    }
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities);
                validate_boundary_change(boundary_identity_history_, current.entities, next.entities,
                                         next.action == "Propagate room relationships");
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, TransformBoundaries>) {
                next.action = typed_command.message.empty() ? "Transform boundaries" : typed_command.message;
                validate_action(next.action);
                next.boundary_transforms = typed_command;
                try {
                    next.entities = boundary_transform_entities(boundary_identity_history_, current.entities, typed_command);
                } catch (const DocumentError&) {
                    throw;
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_entity, error.what());
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities, false, true, true);
                if (same_state(next, current))
                    return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, TranslateBoundaries>) {
                next.action = typed_command.message.empty() ? "Translate boundaries" : typed_command.message;
                validate_action(next.action);
                next.boundary_translations = typed_command;
                next.entities = boundary_translation_entities(boundary_identity_history_, current.entities, typed_command);
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities);
                if (same_state(next, current)) return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, ApplyBoundaryConstraintChanges>) {
                next.action = typed_command.message.empty()
                    ? "Apply boundary constraints" : typed_command.message;
                validate_action(next.action);
                if(typed_command.wall_split)validate_wall_split_lifetime(*typed_command.wall_split,history_,history_.size());
                next.boundary_constraint_changes = typed_command;
                next.entities = boundary_constraint_entities(current.entities, typed_command);
                next.assets = boundary_constraint_assets(current.assets, typed_command);
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_completed_constraint_change(current.entities, next.entities, typed_command);
                try {
                    validate_boundary_identity_transition(
                        boundary_identity_history_, typed_command.wall_split ?
                            wall_split_validation_source(current.entities,next.entities,*typed_command.wall_split) : current.entities, next.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_entity, error.what());
                }
                if (same_state(next, current)) return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, TranslateBoundary>) {
                next.action = "Translate boundary";
                next.boundary_translation = typed_command.translation;
                try {
                    next.entities = translated_boundary_entities(current.entities, typed_command.translation);
                    validate_boundary_identity_transition(boundary_identity_history_, current.entities, next.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_entity, error.what());
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities);
                if (same_state(next, current)) return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, TransformBoundary>) {
                next.action = "Transform boundary";
                next.boundary_transform = typed_command.transformation;
                try {
                    next.entities = transformed_boundary_entities(current.entities, typed_command.transformation);
                    validate_boundary_identity_transition(boundary_identity_history_, current.entities, next.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_entity, error.what());
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities);
                if (same_state(next, current)) return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, EditBoundaryGeometry>) {
                next.action = "Edit boundary geometry";
                next.boundary_geometry_edit = typed_command.edit;
                try {
                    validate_split_dimension_lifetime(typed_command.edit, history_, history_.size());
                    next.entities = edited_boundary_entities(current.entities, typed_command.edit);
                    validate_boundary_identity_transition(
                        boundary_identity_history_, current.entities, next.entities, &typed_command.edit);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_entity, error.what());
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities);
                if (same_state(next, current)) return head_revision_;
                record_boundary_identity_transition(
                    next_identity_history, current.entities, next.entities);
            } else {
                validate_revision_name(typed_command.name);
                if (named_revisions_.contains(typed_command.name)) {
                    document_error(DocumentErrorCode::duplicate_revision_name,
                                   "revision name already exists: " + typed_command.name);
                }
                next.action = "name revision";
                next.name = typed_command.name;
            }

            history_.push_back(std::move(next));
            boundary_identity_history_ = std::move(next_identity_history);
            head_revision_ = history_.back().revision;
            if (next_unsupported_constraints)
                unsupported_constraint_history_reason_ = std::move(next_unsupported_constraints);
            if constexpr (std::is_same_v<CommandType, NameRevision>) {
                named_revisions_.emplace(typed_command.name, head_revision_);
            }
            update_editability();
            return head_revision_;
        },
        command);
}

Revision Document::undo(Revision expected_revision) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    validate_expected_revision(head_revision_, expected_revision);
    const auto current = head_record();
    if (current.undo_stack.empty()) {
        document_error(DocumentErrorCode::no_undo, "there is no command to undo");
    }
    const auto target_revision = current.undo_stack.back();
    const auto& target = history_.at(static_cast<std::size_t>(target_revision));
    if (const auto unsupported = validate_state(target.entities, target.assets))
        document_error(DocumentErrorCode::read_only, *unsupported);
    RevisionRecord next;
    next.revision = static_cast<Revision>(history_.size());
    next.parent_revision = head_revision_;
    next.source_revision = target_revision;
    next.action = "undo";
    next.entities = target.entities;
    next.assets = target.assets;
    next.undo_stack = current.undo_stack;
    next.undo_stack.pop_back();
    next.redo_stack = current.redo_stack;
    next.redo_stack.push_back(head_revision_);
    history_.push_back(std::move(next));
    head_revision_ = history_.back().revision;
    update_editability();
    return head_revision_;
}

Revision Document::redo(Revision expected_revision) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    validate_expected_revision(head_revision_, expected_revision);
    const auto current = head_record();
    if (current.redo_stack.empty()) {
        document_error(DocumentErrorCode::no_redo, "there is no command to redo");
    }
    const auto target_revision = current.redo_stack.back();
    const auto& target = history_.at(static_cast<std::size_t>(target_revision));
    if (const auto unsupported = validate_state(target.entities, target.assets))
        document_error(DocumentErrorCode::read_only, *unsupported);
    RevisionRecord next;
    next.revision = static_cast<Revision>(history_.size());
    next.parent_revision = head_revision_;
    next.source_revision = target_revision;
    next.action = "redo";
    next.entities = target.entities;
    next.assets = target.assets;
    next.undo_stack = current.undo_stack;
    next.undo_stack.push_back(head_revision_);
    next.redo_stack = current.redo_stack;
    next.redo_stack.pop_back();
    history_.push_back(std::move(next));
    head_revision_ = history_.back().revision;
    update_editability();
    return head_revision_;
}

void Document::mark_saved(Revision revision) {
    if (revision >= history_.size() || history_[static_cast<std::size_t>(revision)].revision != revision) {
        document_error(DocumentErrorCode::invalid_saved_revision,
                       "saved revision is absent from document history");
    }
    saved_revision_ = revision;
}

void Document::mark_read_only(std::string reason) {
    if (reason.empty()) reason = "Document is read-only.";
    session_read_only_reason_ = reason;
    editable_ = false;
    read_only_reason_ = std::move(reason);
}

void Document::update_editability() {
    editable_ = true;
    read_only_reason_.clear();
    if (session_read_only_reason_) {
        editable_ = false;
        read_only_reason_ = *session_read_only_reason_;
        return;
    }
    if (unsupported_constraint_history_reason_) {
        editable_ = false;
        read_only_reason_ = *unsupported_constraint_history_reason_;
        return;
    }
    for (const auto& [id, entity] : head_record().entities) {
        if (entity.required && !is_known_entity_type(entity.type)) {
            editable_ = false;
            read_only_reason_ = "required entity type is unsupported: " + entity.type + " (" + id + ")";
            return;
        }
    }
}

Document Document::restore(DocumentSnapshot snapshot) {
    if (!is_valid_identifier(snapshot.document_id_)) {
        document_error(DocumentErrorCode::invalid_history, "stored document id is invalid");
    }
    if (snapshot.history_.empty() || snapshot.revision_ + 1 != snapshot.history_.size()) {
        document_error(DocumentErrorCode::invalid_history, "stored revision history is incomplete");
    }
    if (snapshot.saved_revision_.has_value() && *snapshot.saved_revision_ > snapshot.revision_) {
        document_error(DocumentErrorCode::invalid_history, "stored saved revision is invalid");
    }
    std::map<std::string, Revision, std::less<>> expected_names;
    std::optional<std::string> unsupported_constraint_history;
    BoundaryIdentityHistory identity_history;
    for (std::size_t index = 0; index < snapshot.history_.size(); ++index) {
        const auto& record = snapshot.history_[index];
        validate_action(record.action);
        if (record.revision != index) {
            document_error(DocumentErrorCode::invalid_history, "stored revisions are not contiguous");
        }
        auto unsupported = validate_state(record.entities, record.assets);
        if (unsupported && !unsupported_constraint_history)
            unsupported_constraint_history = std::move(unsupported);

        if (index == 0) {
            if (record.parent_revision.has_value() || record.source_revision.has_value() || record.boundary_translation.has_value() ||
                record.boundary_transform.has_value() || record.boundary_geometry_edit.has_value() ||
                record.boundary_constraint_changes.has_value() || record.boundary_translations.has_value() || record.boundary_transforms.has_value() ||
                record.name.has_value() || record.action != "create" ||
                !record.undo_stack.empty() || !record.redo_stack.empty()) {
                document_error(DocumentErrorCode::invalid_history,
                               "revision zero is not a valid create record");
            }
            record_boundary_identities(identity_history, record.entities);
            continue;
        }

        const auto& previous = snapshot.history_[index - 1];
        const auto boundary_proof_count =
            static_cast<unsigned>(record.boundary_translation.has_value()) +
            static_cast<unsigned>(record.boundary_transform.has_value()) +
            static_cast<unsigned>(record.boundary_geometry_edit.has_value()) +
            static_cast<unsigned>(record.boundary_constraint_changes.has_value()) +
            static_cast<unsigned>(record.boundary_translations.has_value()) +
            static_cast<unsigned>(record.boundary_transforms.has_value());
        if (boundary_proof_count > 1)
            document_error(DocumentErrorCode::invalid_history, "Boundary derivation proofs are mutually exclusive");
        if (record.boundary_transform && (record.name || record.source_revision))
            document_error(DocumentErrorCode::invalid_history,
                           "Boundary transform proof is not valid on history navigation or named revisions");
        if (record.boundary_translation && (record.name || record.source_revision))
            document_error(DocumentErrorCode::invalid_history,
                           "Boundary translation proof is not valid on history navigation or named revisions");
        if (record.boundary_geometry_edit && (record.name || record.source_revision))
            document_error(DocumentErrorCode::invalid_history,
                           "Boundary geometry edit proof is not valid on history navigation or named revisions");
        if (record.boundary_constraint_changes && (record.name || record.source_revision))
            document_error(DocumentErrorCode::invalid_history,
                           "Boundary constraint proof is not valid on history navigation or named revisions");
        if (record.boundary_translations && (record.name || record.source_revision))
            document_error(DocumentErrorCode::invalid_history,
                           "Boundary group translation proof is not valid on history navigation or named revisions");
        if (record.boundary_transforms && (record.name || record.source_revision))
            document_error(DocumentErrorCode::invalid_history,"Boundary group transform proof is not valid on history navigation or named revisions");
        // Unknown locks retain the read-only latch, but must not suppress
        // stable-endpoint checks for known relations in the same history.
        // Undo/redo restores an exact retained state and its provenance. The
        // source-state and stack checks below validate navigation; mutation
        // rules must not reject restoration of a shorter derivation prefix.
        if (record.boundary_constraint_changes && (record.boundary_constraint_changes->wall_split || has_exterior_source_completion(*record.boundary_constraint_changes) ||
            has_rigid_wall_transform(*record.boundary_constraint_changes)))
            validate_completed_constraint_change(previous.entities, record.entities, *record.boundary_constraint_changes, true);
        else validate_constraint_change(previous.entities, record.entities,
                record.boundary_constraint_changes.has_value(), !record.source_revision.has_value(),
                record.boundary_transforms.has_value() || record.source_revision.has_value());
        if (record.parent_revision != Revision{index - 1}) {
            document_error(DocumentErrorCode::invalid_history,
                           "revision parent must be the immediately preceding event");
        }
        if (record.source_revision.has_value() && record.name.has_value()) {
            document_error(DocumentErrorCode::invalid_history,
                           "revision cannot be both named and an undo/redo event");
        }

        if (record.name.has_value()) {
            validate_revision_name(*record.name);
            if (record.action != "name revision" || record.source_revision.has_value() ||
                !same_state(record, previous) ||
                record.undo_stack != appended(previous.undo_stack, Revision{index - 1}) ||
                !record.redo_stack.empty()) {
                document_error(DocumentErrorCode::invalid_history,
                               "named revision transition is impossible");
            }
            if (!expected_names.emplace(*record.name, record.revision).second) {
                document_error(DocumentErrorCode::invalid_history,
                               "stored revision name is duplicated");
            }
        } else if (record.source_revision.has_value()) {
            if (*record.source_revision >= index) {
                document_error(DocumentErrorCode::invalid_history,
                               "undo/redo source revision is not prior history");
            }
            const auto& source = snapshot.history_[static_cast<std::size_t>(*record.source_revision)];
            if (!same_state(record, source)) {
                document_error(DocumentErrorCode::invalid_history,
                               "undo/redo state does not match its source revision");
            }
            if (record.action == "undo") {
                if (previous.undo_stack.empty() ||
                    *record.source_revision != previous.undo_stack.back()) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "undo source is not the prior undo-stack top");
                }
                auto expected_undo = previous.undo_stack;
                expected_undo.pop_back();
                if (record.undo_stack != expected_undo ||
                    record.redo_stack != appended(previous.redo_stack, Revision{index - 1})) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "undo navigation transition is impossible");
                }
            } else if (record.action == "redo") {
                if (previous.redo_stack.empty() ||
                    *record.source_revision != previous.redo_stack.back()) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "redo source is not the prior redo-stack top");
                }
                auto expected_redo = previous.redo_stack;
                expected_redo.pop_back();
                if (record.undo_stack != appended(previous.undo_stack, Revision{index - 1}) ||
                    record.redo_stack != expected_redo) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "redo navigation transition is impossible");
                }
            } else {
                document_error(DocumentErrorCode::invalid_history,
                               "source revision is only valid for undo or redo");
            }
        } else if (record.undo_stack != appended(previous.undo_stack, Revision{index - 1}) ||
                   !record.redo_stack.empty()) {
            document_error(DocumentErrorCode::invalid_history,
                           "apply-command navigation transition is impossible");
        }
        // An exact, validated history navigation may undo an identity upgrade.
        // Ordinary Apply records must never masquerade as that downgrade.
        if (!record.source_revision.has_value()) {
            if (record.boundary_transforms) {
                const auto& proof = *record.boundary_transforms;
                const auto action = proof.message.empty() ? "Transform boundaries" : proof.message;
                if (proof.expected_revision != previous.revision || record.action != action)
                    document_error(DocumentErrorCode::invalid_history, "Boundary transform group proof does not match revision");
                auto expected = previous;
                try {
                    expected.entities = boundary_transform_entities(identity_history, previous.entities, proof);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history, error.what());
                }
                if (!same_state(expected, record) || same_state(expected, previous))
                    document_error(DocumentErrorCode::invalid_history, "Boundary transform group differs from deterministic reconstruction");
            } else if (record.boundary_translations) {
                const auto& proof = *record.boundary_translations;
                const auto action = proof.message.empty() ? "Translate boundaries" : proof.message;
                if (proof.expected_revision != previous.revision || record.action != action)
                    document_error(DocumentErrorCode::invalid_history, "Boundary translation group proof does not match revision");
                auto expected = previous;
                try { expected.entities = boundary_translation_entities(identity_history, previous.entities, proof); }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_history, error.what()); }
                if (!same_state(expected, record) || same_state(expected, previous))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary translation group differs from deterministic reconstruction");
            } else if (record.boundary_translation) {
                if (record.action != "Translate boundary")
                    document_error(DocumentErrorCode::invalid_history, "Boundary translation action does not match proof");
                auto expected = previous;
                try {
                    expected.entities = translated_boundary_entities(previous.entities, *record.boundary_translation);
                    validate_boundary_identity_transition(identity_history, previous.entities, record.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history, error.what());
                }
                if (!same_state(expected, record))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary translation state differs from deterministic reconstruction");
                if (same_state(expected, previous))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Unchanged boundary translation cannot create a history record");
            } else if (record.boundary_transform) {
                if (record.action != "Transform boundary")
                    document_error(DocumentErrorCode::invalid_history, "Boundary transform action does not match proof");
                auto expected = previous;
                try {
                    expected.entities = transformed_boundary_entities(previous.entities, *record.boundary_transform);
                    validate_boundary_identity_transition(identity_history, previous.entities, record.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history, error.what());
                }
                if (!same_state(expected, record))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary transform state differs from deterministic reconstruction");
                if (same_state(expected, previous))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Unchanged boundary transform cannot create a history record");
            } else if (record.boundary_geometry_edit) {
                if (record.action != "Edit boundary geometry")
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary geometry edit action does not match proof");
                auto expected = previous;
                try {
                    validate_split_dimension_lifetime(*record.boundary_geometry_edit, snapshot.history_, index);
                    expected.entities = replayed_boundary_entities(
                        previous.entities, *record.boundary_geometry_edit);
                    validate_boundary_identity_transition(
                        identity_history, previous.entities, record.entities, &*record.boundary_geometry_edit);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history, error.what());
                }
                if (!same_state(expected, record))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary geometry state differs from deterministic reconstruction");
                if (same_state(expected, previous))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Unchanged boundary geometry edit cannot create a history record");
            } else if (record.boundary_constraint_changes) {
                const auto& proof = *record.boundary_constraint_changes;
                const auto action = proof.message.empty() ? "Apply boundary constraints" : proof.message;
                if (proof.expected_revision != previous.revision || record.action != action)
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary constraint transaction proof does not match revision");
                auto expected = previous;
                try {
                    if(proof.wall_split)validate_wall_split_lifetime(*proof.wall_split,snapshot.history_,index);
                    expected.entities = boundary_constraint_entities(previous.entities, proof, true);
                    expected.assets = boundary_constraint_assets(previous.assets, proof);
                    validate_boundary_identity_transition(identity_history, proof.wall_split ?
                        wall_split_validation_source(previous.entities,expected.entities,*proof.wall_split) : previous.entities, record.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history, error.what());
                }
                if (!same_state(expected, record) || same_state(expected, previous))
                    document_error(DocumentErrorCode::invalid_history,
                                   "Boundary constraint state differs from deterministic reconstruction");
            } else validate_boundary_change(identity_history, previous.entities, record.entities,
                                            record.action == "Propagate room relationships");
            record_boundary_identity_transition(identity_history, previous.entities, record.entities);
        } else record_boundary_identities(identity_history, record.entities);
    }
    if (expected_names != snapshot.named_revisions_) {
        document_error(DocumentErrorCode::invalid_history,
                       "named revision index is not a bijection with history");
    }
    Document document(std::move(snapshot.document_id_));
    document.head_revision_ = snapshot.revision_;
    document.saved_revision_ = snapshot.saved_revision_;
    document.history_ = std::move(snapshot.history_);
    document.named_revisions_ = std::move(snapshot.named_revisions_);
    document.unsupported_constraint_history_reason_ = std::move(unsupported_constraint_history);
    if (!snapshot.editable_ && !snapshot.read_only_reason_.empty()) {
        document.session_read_only_reason_ = snapshot.read_only_reason_;
    }
    document.boundary_identity_history_ = std::move(identity_history);
    document.update_editability();
    return document;
}

}  // namespace sketch
