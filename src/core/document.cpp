#include "sketch/document.hpp"
#include "sketch/sha256_stream.hpp"
#include "sketch/dxf_source_receipt.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/corner_window_edit.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/room_relationship_geometry.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/reference_grid.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/presentation_transform.hpp"
#include "sketch/georeferencing_entity_codec.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_split.hpp"
#include "sketch/wall_merge.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/survey_source_version.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/physical_wall_phase_review.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/mixed_wall_opening_removal.hpp"
#endif
#include "sketch/joint_translation_replay.hpp"
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
#include "sketch/drawing_selection_removal.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_coordinated_demolition.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/phase_roof_uniform_transform.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/phase_slab_replacement.hpp"
#include "sketch/phase_structural_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_stair_demolition_retirement.hpp"
#include "sketch/phase_stair_replacement.hpp"
#include "sketch/slab_layer_stack_edit.hpp"
#include "sketch/wall_layer_stack_edit.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/phase_wall_demolition_authoring.hpp"
#include "sketch/wall_join_removal.hpp"
#endif

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
#include <cstddef>
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
#include <utility>

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
    if (!value.is_object() ||
        !value.contains("version") || !value.contains("graph_id") ||
        !value.contains("link_id") || !value.contains("lower_level_id") ||
        !value.contains("upper_level_id")) {
        document_error(DocumentErrorCode::invalid_entity,
                       "stair level_connection requires version, graph_id, link_id, lower_level_id, and upper_level_id");
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
    try {
        (void)measurement_linework_copy_isolated(entity);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity,
                       "invalid measured-copy source scope " + entity.id + ": " + error.what());
    }
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
    if (entity.extensions.contains("physical_wall_room")) {
        try { (void)validate_physical_wall_room_descriptor(entity); }
        catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity, error.what());
        }
    }
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
    if (entity.type == "wall" && entity.properties.contains("top_plane")) {
        try {
            (void)parse_wall_top_plane(entity.properties.at("top_plane"));
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid wall top plane: ") + error.what());
        }
    }
    if (entity.properties.contains("opening_assembly")) {
        if (entity.type != "opening" && entity.type != "corner_window") {
            document_error(DocumentErrorCode::invalid_entity,
                           "Opening assembly requires an opening or corner window entity");
        }
        try {
            const auto assembly = parse_opening_assembly(
                entity.properties.at("opening_assembly"));
            const auto kind = entity.properties.find("opening_kind");
            if (entity.type == "opening" && (kind == entity.properties.end() || !kind->is_string() ||
                kind->get<std::string>() != opening_assembly_kind_name(assembly.kind))) {
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
    if (entity.type == "corner_window") {
        try { (void)parse_corner_window(entity); }
        catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity, error.what());
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
    if (entity.type == "wall" && entity.extensions.contains("wall_layer_stack_retirement")) {
        try {
            const auto& archive = entity.extensions.at("wall_layer_stack_retirement");
            if (!archive.is_object() || !archive.contains("version") ||
                !archive.at("version").is_number_integer() ||
                (archive.at("version").is_number_unsigned() ? archive.at("version").get<std::uint64_t>() == 0 :
                    archive.at("version").get<std::int64_t>() <= 0))
                throw std::invalid_argument("Wall layer retirement requires a positive version");
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            if (archive.at("version") == 1) validate_wall_layer_stack_retirement(archive);
#endif
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                std::string("invalid wall layer retirement: ") + error.what());
        }
    }
    if (entity.type == "slab" && entity.extensions.contains("slab_geometry_derivations")) {
        try {
            const auto& archive = entity.extensions.at("slab_geometry_derivations");
            if (!archive.is_object() || !archive.contains("version") ||
                !archive.at("version").is_number_integer() ||
                (archive.at("version").is_number_unsigned() ? archive.at("version").get<std::uint64_t>() == 0 :
                    archive.at("version").get<std::int64_t>() <= 0))
                throw std::invalid_argument("Horizontal geometry derivation requires a positive version");
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            if (archive.at("version") == 1 || archive.at("version") == 2) validate_slab_geometry_derivation(entity);
#endif
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                std::string("invalid horizontal geometry derivation: ") + error.what());
        }
    }
    for (const auto* key : {"roof_rigid_transform_derivations", "roof_plan_resize_derivations",
                          "roof_uniform_transform_derivations"})
    if (entity.type == "roof" && entity.extensions.contains(key)) {
        try {
            const auto& archive = entity.extensions.at(key);
            if (!archive.is_object() || !archive.contains("version") ||
                !archive.at("version").is_number_integer() ||
                (archive.at("version").is_number_unsigned() ? archive.at("version").get<std::uint64_t>() == 0 :
                    archive.at("version").get<std::int64_t>() <= 0))
                throw std::invalid_argument("Roof mathematical derivation requires a positive version");
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            if (archive.at("version") == 1) {
                if (std::string_view(key) == roof_rigid_transform_derivations_key)
                    validate_roof_rigid_transform_derivations(entity);
                else if (std::string_view(key) == roof_plan_resize_derivations_key)
                    validate_roof_plan_resize_derivations(entity);
                else validate_roof_uniform_transform_derivations(entity);
            }
#endif
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                std::string("invalid roof mathematical derivation: ") + error.what());
        }
    }
    if (entity.type == "roof_join") {
        try {
            (void)parse_roof_join(entity.properties, entity.id);
            (void)has_phase_qualified_roof_join_ownership(entity);
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
    if (entity.type == "assembly_instance") {
        try { (void)decode_document_assembly_instance(entity); }
        catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                std::string("invalid independent assembly instance: ") + error.what());
        }
    } else if (entity.type == "assembly_model") {
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
        asset.bytes.verified_sha256() != asset.sha256) {
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
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 20> typed{{
        {"assembly_catalog_id", "assembly_model"},
        {"property_id", "property"},
        {"building_id", "building"},
        {"floor_id", "floor"},
        {"layer_id", "layer"},
        {"boundary_id", "boundary"},
        {"wall_id", "wall"},
        {"opening_id", "opening"},
        {"corner_window_id", "corner_window"},
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

using ReceiptOwners = std::map<std::string, Entity, std::less<>>;
using ReceiptAssets = std::map<std::string, Asset, std::less<>>;

struct DxfReceiptValidationCache {
    struct Entry {
        const Entity* owner{};
        std::vector<const nlohmann::json*> dependencies;
        NativeDxfPhaseSourceAssetRefs assets;
    };
    std::map<std::string, Entry, std::less<>> entries;
};

bool receipt_raw_equal(const nlohmann::json& left, const nlohmann::json& right) {
    if (left.type() != right.type() || left.size() != right.size()) return false;
    if (left.is_object()) {
        auto other = right.begin();
        for (auto it = left.begin(); it != left.end(); ++it, ++other)
            if (it.key() != other.key() || !receipt_raw_equal(it.value(), other.value())) return false;
        return true;
    }
    if (left.is_array()) {
        for (std::size_t i = 0; i < left.size(); ++i)
            if (!receipt_raw_equal(left[i], right[i])) return false;
        return true;
    }
    return left == right;
}

bool dxf_dependency_segment(const Entity& entity) {
    return entity.type == "dxf_source" && entity.properties.contains("schema") &&
        entity.properties.at("schema") == "vertex.dxf.source-dependencies.v1";
}

void validate_dxf_source_receipts(const ReceiptOwners& entities, const ReceiptAssets& assets,
    DxfReceiptValidationCache* cache = nullptr, bool seed_verified_state = false) {
    try {
        std::map<std::string, const Entity*, std::less<>> receipts;
        std::map<std::string, std::map<std::uint64_t, const nlohmann::json*>, std::less<>> segments;
        const auto identity = [](const nlohmann::json& value) {
            if (!value.is_string() || !is_valid_identifier(value.get_ref<const std::string&>()))
                document_error(DocumentErrorCode::invalid_entity, "Invalid DXF source receipt identity");
            return value.get<std::string>();
        };
        std::size_t receipt_rows = 0;
        for (const auto& [id, entity] : entities) {
            (void)id;
            if (entity.type != "dxf_source") continue;
            if (entity.properties.contains("source_receipt")) {
                if (++receipt_rows > native_dxf_phase_destination_asset_count_limit ||
                    !entity.properties.at("source_receipt").is_object())
                    document_error(DocumentErrorCode::invalid_entity, "Invalid DXF source receipt inventory");
                const auto recipe = identity(entity.properties.at("source_receipt").at("recipe_asset_id"));
                if (!receipts.emplace(recipe, &entity).second)
                    document_error(DocumentErrorCode::invalid_entity, "Duplicate DXF source receipt recipe");
            }
            if (dxf_dependency_segment(entity)) {
                if (++receipt_rows > native_dxf_phase_destination_asset_count_limit ||
                    !entity.properties.at("segment_index").is_number_unsigned())
                    document_error(DocumentErrorCode::invalid_entity, "Invalid DXF source dependency ordinal");
                const auto recipe = identity(entity.properties.at("recipe_asset_id"));
                const auto index = entity.properties.at("segment_index").get<std::uint64_t>();
                if (!segments[recipe].emplace(index, &entity.properties).second)
                    document_error(DocumentErrorCode::invalid_entity, "Duplicate DXF source dependency segment");
            }
        }
        for (const auto& [recipe, group] : segments) {
            (void)group;
            if (!receipts.contains(recipe))
                document_error(DocumentErrorCode::dangling_reference, "DXF source dependencies have no receipt");
        }
        NativeDxfPhaseAssetWorkBudget budget;
        for (const auto& [recipe, entity] : receipts) {
            NativeDxfPhaseSourceAssetRefs retained;
            std::vector<const nlohmann::json*> dependencies;
            const auto collect = [&](const nlohmann::json& properties) {
                const auto& ids = properties.at("asset_ids");
                if (!ids.is_array() || ids.size() > native_dxf_phase_destination_asset_count_limit)
                    document_error(DocumentErrorCode::invalid_entity, "Invalid DXF source dependency references");
                for (const auto& value : ids) {
                    const auto id = identity(value);
                    const auto asset = assets.find(id);
                    if (asset == assets.end() || !retained.emplace(id, &asset->second).second)
                        document_error(DocumentErrorCode::dangling_reference, "Missing or repeated DXF source dependency");
                }
            };
            collect(entity->properties);
            const auto group = segments.find(recipe);
            if (group != segments.end()) {
                std::uint64_t next = 0;
                for (const auto& [index, properties] : group->second) {
                    if (index != next++)
                        document_error(DocumentErrorCode::invalid_entity, "Incomplete DXF source dependency segments");
                    dependencies.push_back(properties);
                    collect(*properties);
                }
            }
            bool verified = seed_verified_state;
            if (cache && !verified) {
                const auto previous = cache->entries.find(recipe);
                if (previous != cache->entries.end()) {
                    const auto& entry = previous->second;
                    verified = receipt_raw_equal(entity->properties.at("source_receipt"),
                        entry.owner->properties.at("source_receipt")) &&
                        receipt_raw_equal(entity->properties.at("asset_ids"), entry.owner->properties.at("asset_ids")) &&
                        dependencies.size() == entry.dependencies.size() && retained.size() == entry.assets.size();
                    for (std::size_t i = 0; verified && i < dependencies.size(); ++i)
                        verified = receipt_raw_equal(*dependencies[i], *entry.dependencies[i]);
                    for (const auto& [id, asset] : retained) {
                        if (!verified) break;
                        const auto old = entry.assets.find(id);
                        verified = old != entry.assets.end() && asset->bytes.same_storage(old->second->bytes) &&
                            asset->id == old->second->id && asset->media_type == old->second->media_type &&
                            asset->sha256 == old->second->sha256 && receipt_raw_equal(asset->metadata, old->second->metadata);
                    }
                }
            }
            if (!verified) validate_native_dxf_source_receipt(entity->properties, dependencies, retained, &budget);
            if (cache) cache->entries.insert_or_assign(recipe,
                DxfReceiptValidationCache::Entry{entity, std::move(dependencies), std::move(retained)});
        }
    } catch (const DocumentError&) { throw; }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, std::string("Invalid retained DXF source: ") + error.what());
    }
}

void validate_dxf_source_receipt_transition(const ReceiptOwners& before, const ReceiptAssets& before_assets,
    const ReceiptOwners& after, const ReceiptAssets& after_assets) {
    for (const auto& [id, entity] : before) {
        if (entity.type != "dxf_source") continue;
        const bool main = entity.properties.contains("source_receipt");
        const bool segment = dxf_dependency_segment(entity);
        if (!main && !segment) continue;
        const auto current = after.find(id);
        if (current == after.end()) continue; // Explicit complete receipt removal is reversible through history.
        if (current->second.type != "dxf_source" ||
            (main && (!current->second.properties.contains("source_receipt") ||
                current->second.properties.at("source_receipt") != entity.properties.at("source_receipt") ||
                current->second.properties.at("asset_ids") != entity.properties.at("asset_ids"))) ||
            (segment && current->second.properties != entity.properties))
            document_error(DocumentErrorCode::invalid_entity, "Retained DXF source receipts are immutable");
        for (const auto& value : entity.properties.at("asset_ids")) {
            const auto asset_id = value.get<std::string>();
            const auto previous = before_assets.find(asset_id);
            const auto retained = after_assets.find(asset_id);
            if (previous == before_assets.end() || retained == after_assets.end() ||
                previous->second.sha256 != retained->second.sha256 || previous->second.bytes != retained->second.bytes)
                document_error(DocumentErrorCode::invalid_asset, "Retained DXF source payloads cannot be replaced");
        }
    }
}

std::optional<std::string> validate_state(const std::map<std::string, Entity, std::less<>>& entities,
                    const std::map<std::string, Asset, std::less<>>& assets,
                    bool active_phase_constraints = false, DxfReceiptValidationCache* receipt_cache = nullptr) {
    try {
        validate_stair_attachment_state(entities);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
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
                if(entity.extensions.contains("wall_merge_archive")) {
                    const auto& archive=entity.extensions.at("wall_merge_archive");
                    if(!archive.is_object() || !archive.contains("version") || !archive.at("version").is_number_integer() ||
                        archive.at("version").get<std::int64_t>()<=0)
                        throw std::invalid_argument("Wall merge archive requires a positive version: "+id);
                    if(archive.at("version")==1)validate_wall_merge_archive(entity);
                }
            }
            catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
        }
        if (id != entity.id) {
            document_error(DocumentErrorCode::invalid_entity,
                           "entity map key does not match its stable id");
        }
    }
    try {
        validate_corner_window_state(entities);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    for (const auto& [id, asset] : assets) {
        validate_asset(asset);
        if (id != asset.id) {
            document_error(DocumentErrorCode::invalid_asset,
                           "asset map key does not match its stable id");
        }
    }
    validate_dxf_source_receipts(entities, assets, receipt_cache);
    std::optional<ConstraintPhaseScope> active_scope;
    if (active_phase_constraints) {
        try { active_scope = constraint_phase_scope(entities); }
        catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity, error.what());
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
                                "view dimension " + overlay.id + " references missing object " + target_id);
                        }
                        static constexpr std::array<std::string_view, 12> dimension_types{
                            "wall", "opening", "room", "slab", "roof", "stair", "railing",
                            "column", "beam", "wall_join", "roof_join", "assembly_instance"};
                        if (std::find(dimension_types.begin(), dimension_types.end(), target->second.type) == dimension_types.end()) {
                            document_error(DocumentErrorCode::invalid_entity,
                                "view dimension " + overlay.id + " references unsupported object " + target_id);
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
    try {
        validate_roof_join_ownership(entities);
    } catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    std::map<std::string, AssemblyModel> material_catalogs;
    for (const auto& [id, entity] : entities) {
        if (entity.properties.contains("door_operation")) {
            try {
                if (entity.type != "opening")
                    document_error(DocumentErrorCode::invalid_entity, "Door operation requires an opening");
                const auto operation = decode_door_operation(entity.properties.at("door_operation"));
                if (uses_door_opening_fraction(operation.kind)) {
                    if (!entity.properties.contains("opening_assembly") ||
                        parse_opening_assembly(entity.properties.at("opening_assembly")).kind != OpeningAssemblyKind::door)
                        document_error(DocumentErrorCode::invalid_entity,
                            "This door operation requires an explicit door assembly");
                    std::string wall_id, error;
                    if (!read_document_wall_id(entity, wall_id, error))
                        document_error(DocumentErrorCode::invalid_entity, error);
                    const auto wall = entities.find(wall_id);
                    Wall host;
                    if (wall == entities.end() || wall->second.type != "wall" ||
                        !read_document_wall(wall->second, {}, host, error))
                        document_error(DocumentErrorCode::invalid_entity,
                            "This door operation requires an actual valid wall");
                    if (host.baseline.sweep_radians != 0)
                        document_error(DocumentErrorCode::invalid_entity,
                            "This door operation requires a straight wall");
                }
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, std::string("Invalid door operation: ") + error.what());
            }
        }
        if (entity.properties.contains("material_assignment")) {
            try {
                static constexpr std::array<std::string_view, 12> roles{
                    "wall", "opening", "corner_window", "room", "room_boundary", "slab", "roof", "stair", "railing", "column", "beam", "roof_join"};
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
                    const auto& type = entities.at(member_id).type;
                    if (!is_model_phase_entity_type(type)) {
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
                            if (active_scope && !active_scope->inactive_owner_ids.contains(member)) {
                                // Decode every retained descriptor against the
                                // complete graph before collecting saved-active
                                // cuts. Inactive cuts have no current host fit or
                                // aggregate overlap, but remain admitted records.
                                std::erase_if(wall.openings, [&](const auto& opening) {
                                    if (!active_scope->inactive_owner_ids.contains(opening.id)) return false;
                                    if (opening.id.empty() ||
                                        opening.width <= default_geometry_tolerance_metres ||
                                        opening.height <= default_geometry_tolerance_metres ||
                                        opening.offset < 0.0 || opening.sill < 0.0 ||
                                        !std::isfinite(opening.offset + opening.width) ||
                                        !std::isfinite(opening.sill + opening.height))
                                        document_error(DocumentErrorCode::invalid_entity,
                                            "Invalid retained opening descriptor: " + opening.id);
                                    return true;
                                });
                            }
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
    try { validate_document_assembly_instances(entities); }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity,
            std::string("Invalid complete assembly graph: ") + error.what());
    }
    try { validate_document_site_frames(entities); }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity,
            std::string("Invalid site presentation frames: ") + error.what());
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
    for (const auto& [id, entity] : entities) {
        if (!entity.extensions.contains("physical_wall_room")) continue;
        if (const auto unsupported = validate_physical_wall_room_descriptor(entity))
            unsupported_boundary = *unsupported + ": " + id;
    }
    for(const auto& [id,entity]:entities)
        if(entity.type=="wall" && entity.extensions.contains("wall_split_archive") &&
            entity.extensions.at("wall_split_archive").at("version")!=1)
            unsupported_boundary="Unsupported wall split archive: "+id;
    for(const auto& [id,entity]:entities)
        if(entity.type=="wall" && entity.extensions.contains("wall_merge_archive") &&
            entity.extensions.at("wall_merge_archive").at("version")!=1)
            unsupported_boundary="Unsupported wall merge archive: "+id;
    for (const auto& [id, entity] : entities)
        for (const auto* key : {"roof_rigid_transform_derivations", "roof_plan_resize_derivations",
                              "roof_uniform_transform_derivations"})
        if (entity.type == "roof" && entity.extensions.contains(key)) {
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            if (entity.extensions.at(key).at("version") != 1)
#endif
                unsupported_boundary = std::string("Unsupported roof mathematical derivation ") + key + ": " + id;
        }
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
        const auto unsupported_constraint = active_phase_constraints ?
            validate_active_phase_constraint_integrity(entities) : validate_constraint_integrity(entities);
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
                                const std::set<std::string,std::less<>>& verified_rigid_wall_ids={},
                                const std::set<std::string,std::less<>>& curve_construction_owner_ids={}) {
    try {
        validate_constraint_transition(before, after, qualified_rigid_transform, verified_rigid_wall_ids);
        if (validate_curve_provenance)
            validate_constraint_wall_geometry_transition(before,after,qualified_curve_edits,false,curve_construction_owner_ids);
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

void validate_phase_entity_import_transition(const RevisionRecord& source,
    const RevisionRecord& candidate, const PhaseEntityImportProof& proof,
    std::span<const RevisionRecord> retained) {
    (void)phase_entity_import_proof_to_json(proof);
    if (proof.expected_revision!=source.revision)
        document_error(DocumentErrorCode::stale_revision,"Phase import source revision changed");
    const std::set<std::string,std::less<>> imported(proof.entity_ids.begin(),proof.entity_ids.end());
    const std::set<std::string,std::less<>> assets(proof.asset_ids.begin(),proof.asset_ids.end());
    const std::set<std::string,std::less<>> registries(proof.registry_ids.begin(),proof.registry_ids.end());
    const std::set<std::string,std::less<>> reviewed_hierarchy(
        proof.reviewed_existing_hierarchy_ids.begin(),proof.reviewed_existing_hierarchy_ids.end());
    if (candidate.entities.size()!=source.entities.size()+imported.size() ||
        candidate.assets.size()!=source.assets.size()+assets.size())
        document_error(DocumentErrorCode::invalid_entity,"Phase import must retain exactly its listed fresh additions");
    for (const auto& [id,entity]:source.entities) {
        if (entity.required && !is_known_entity_type(entity.type))
            document_error(DocumentErrorCode::read_only,"Phase import source contains an unsupported required entity: "+id);
        const auto after=candidate.entities.find(id);
        if (after==candidate.entities.end() || entity!=after->second ||
            entity.properties.dump()!=after->second.properties.dump() ||
            entity.extensions.dump()!=after->second.extensions.dump())
            document_error(DocumentErrorCode::invalid_entity,"Phase import cannot change an existing entity: "+id);
    }
    for (const auto& [id,asset]:source.assets) {
        const auto after=candidate.assets.find(id);
        if (after==candidate.assets.end() || asset!=after->second ||
            asset.metadata.dump()!=after->second.metadata.dump())
            document_error(DocumentErrorCode::invalid_asset,"Phase import cannot change an existing asset: "+id);
    }
    for (const auto& record:retained) {
        for (const auto& [id,entity]:record.entities) {
            (void)entity;
            if (imported.contains(id) || assets.contains(id))
                document_error(DocumentErrorCode::invalid_entity,"Phase import identity was already retained: "+id);
        }
        for (const auto& [id,asset]:record.assets) {
            (void)asset;
            if (imported.contains(id) || assets.contains(id))
                document_error(DocumentErrorCode::invalid_asset,"Phase import identity was already retained: "+id);
        }
    }
    for (const auto& id:assets)
        if (!candidate.assets.contains(id) || imported.contains(id))
            document_error(DocumentErrorCode::invalid_asset,"Phase import asset inventory is invalid: "+id);
    if (!reviewed_hierarchy.empty()) {
        for (const auto& id:reviewed_hierarchy) {
            const auto existing=source.entities.find(id);
            if (existing==source.entities.end() ||
                (existing->second.type!="building" && existing->second.type!="floor") ||
                imported.contains(id) || assets.contains(id))
                document_error(DocumentErrorCode::invalid_entity,"Reviewed phase import enrollment requires an existing building or floor: "+id);
        }
        for (const auto& [id,entity]:source.entities) {
            (void)id;
            if (entity.type!="model_phases") continue;
            const auto model=ModelPhases::from_json(entity.properties.at("model"));
            for (const auto& owner:model.entity_ids())
                if (reviewed_hierarchy.contains(owner))
                    document_error(DocumentErrorCode::invalid_entity,"Reviewed phase import hierarchy is already owned by a source registry: "+owner);
        }
    }
    std::set<std::string,std::less<>> actual_registries;
    std::set<std::string,std::less<>> actual_reviewed_hierarchy;
    std::optional<ProjectOrganization> annotation_organization;
    std::optional<std::map<std::string,std::size_t,std::less<>>> imported_view_ids;
    std::optional<std::set<std::string,std::less<>>> existing_view_ids;
    for (const auto& id:imported) {
        const auto found=candidate.entities.find(id);
        if (found==candidate.entities.end())
            document_error(DocumentErrorCode::invalid_entity,"Phase import entity inventory is absent: "+id);
        const auto& entity=found->second;
        if (entity.type=="model_phases") {
            actual_registries.insert(id);
            const auto model=ModelPhases::from_json(entity.properties.at("model"));
            for (const auto& owner:model.entity_ids()) {
                if (imported.contains(owner)) continue;
                if (!reviewed_hierarchy.contains(owner))
                    document_error(DocumentErrorCode::invalid_entity,"Phase import registry requires a fresh owner or explicitly reviewed existing hierarchy: "+owner);
                actual_reviewed_hierarchy.insert(owner);
            }
        }
        // Equivalent destination hierarchy/levels may be bound by the caller.
        // Semantic hosts, catalogs and analytical targets travel in this import.
        std::vector<EntityReference> references;
        collect_references(entity,references);
        for (const auto& reference:references) {
            if (reference.target==EntityReference::Target::asset) {
                if (!assets.contains(reference.id))
                    document_error(DocumentErrorCode::invalid_asset,"Phase import requires its fresh referenced asset: "+reference.id);
                continue;
            }
            if (imported.contains(reference.id)) continue;
            const auto destination=source.entities.find(reference.id);
            if (destination==source.entities.end() ||
                (destination->second.type!="property" && destination->second.type!="building" &&
                 destination->second.type!="floor" && destination->second.type!="layer" &&
                 destination->second.type!="vertical_levels"))
                document_error(DocumentErrorCode::invalid_entity,"Phase import semantic dependencies must be fresh: "+reference.id);
        }
        if (entity.type==kSheetViewEntityType) {
            const auto model=decode_sheet_view_entity(entity);
            if (!existing_view_ids) {
                existing_view_ids.emplace();
                for (const auto& [owner_id,owner]:source.entities) {
                    (void)owner_id;
                    if (owner.type==kSheetViewEntityType) {
                        const auto existing_model=decode_sheet_view_entity(owner);
                        for (const auto& view:existing_model.views()) existing_view_ids->insert(view.id);
                    }
                    if (owner.type==kAnnotationEntityType)
                        for (const auto& row:owner.properties.at("state").at("overrides"))
                            if (row.at("target_kind")=="output_view")
                                existing_view_ids->insert(row.at("target_id").get<std::string>());
                }
            }
            const auto require_fresh_target=[&](const std::string& target) {
                if (imported.contains(target)) return;
                const auto existing=source.entities.find(target);
                if (existing!=source.entities.end() &&
                    (existing->second.type=="property" || existing->second.type=="building" ||
                     existing->second.type=="floor" || existing->second.type=="layer" ||
                     existing->second.type=="vertical_levels")) return;
                document_error(DocumentErrorCode::invalid_entity,"Phase import sheet semantic owner must be fresh: "+target);
            };
            for (const auto& view:model.views()) {
                if (existing_view_ids->contains(view.id))
                    document_error(DocumentErrorCode::invalid_entity,"Phase import view shadows an existing output view: "+view.id);
                for (const auto& target:view.object_ids) require_fresh_target(target);
                if (view.presentation.appearance)
                    for (const auto& object:view.presentation.appearance->objects) require_fresh_target(object.object_id);
                for (const auto& overlay:view.overlays) {
                    if (!overlay.object_id.empty() && candidate.entities.contains(overlay.object_id))
                        require_fresh_target(overlay.object_id);
                    if (overlay.dimension_binding) require_fresh_target(overlay.dimension_binding->object_id);
                }
            }
        } else if (entity.type=="constraint") {
            const auto decoded=decode_constraint_entity(entity);
            if (!decoded.supported())
                document_error(DocumentErrorCode::invalid_entity,"Phase import requires supported constraint bindings: "+id);
            for (const auto& binding:decoded.constraint->bindings)
                if (!imported.contains(binding.owner_id))
                    document_error(DocumentErrorCode::invalid_entity,"Phase import constraint owner must be fresh: "+binding.owner_id);
        } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (!decoded.supported() || !imported.contains(decoded.dimension->boundary_id))
                document_error(DocumentErrorCode::invalid_entity,"Phase import dimension must bind a supported fresh owner: "+id);
        } else if (entity.type==kAnnotationEntityType) {
            const auto state=decode_annotation_entity(entity);
            if (!annotation_organization) annotation_organization=organize_project(candidate.entities);
            const auto& organization=*annotation_organization;
            const auto layer_context=[&](const std::string& layer_id) -> DrawingContext {
                const auto layer=candidate.entities.find(layer_id);
                const auto node=organization.nodes.find(layer_id);
                if (layer==candidate.entities.end() || layer->second.type!="layer" ||
                    node==organization.nodes.end() || !node->second.issues.empty() || !node->second.context.complete())
                    document_error(DocumentErrorCode::invalid_entity,"Phase import annotation layer hierarchy is unresolved: "+layer_id);
                return node->second.context;
            };
            if (entity.properties.contains("layer_id")) {
                const auto context=layer_context(entity.properties.at("layer_id").get<std::string>());
                for (const auto& [slot,expected]:{
                    std::pair{"property_id",&context.property_id},std::pair{"building_id",&context.building_id},
                    std::pair{"floor_id",&context.floor_id}})
                    if (entity.properties.at(slot).get<std::string>()!=*expected)
                        document_error(DocumentErrorCode::invalid_entity,"Phase import annotation owner hierarchy conflicts: "+id);
                if (entity.properties.contains("level_id") && entity.properties.at("level_id").get<std::string>()!=context.level_id)
                    document_error(DocumentErrorCode::invalid_entity,"Phase import annotation owner level conflicts: "+id);
            }
            std::set<std::string,std::less<>> children;
            const auto child=[&](const auto& value) {
                if (!children.insert(value.id).second)
                    document_error(DocumentErrorCode::invalid_entity,"Phase import annotation child identities overlap: "+id);
                if (!value.placement.layer_id.empty()) (void)layer_context(value.placement.layer_id);
            };
            for (const auto& label:state.labels) child(label);
            for (const auto& symbol:state.symbols) child(symbol);
            const auto appearance_owner=[](std::string_view type) {
                return type=="wall" || type=="opening" || type=="slab" || type=="room" ||
                    type=="assembly_instance" || type=="roof_join" || type=="column" || type=="beam" ||
                    type=="stair" || type=="railing" || type=="roof";
            };
            for (const auto& row:state.overrides) {
                const auto owner=candidate.entities.find(row.target_id);
                if (row.target_kind=="object" && children.contains(row.target_id) &&
                    (owner==candidate.entities.end() || !appearance_owner(owner->second.type))) continue;
                if (row.target_kind=="output_view") {
                    // View IDs live in their typed companion's local namespace.
                    // Require an unambiguous freshly imported companion.
                    if (!imported_view_ids) {
                        imported_view_ids.emplace();
                        for (const auto& companion_id:imported) {
                            const auto& companion=candidate.entities.at(companion_id);
                            if (companion.type!=kSheetViewEntityType) continue;
                            const auto model=decode_sheet_view_entity(companion);
                            for (const auto& view:model.views()) ++(*imported_view_ids)[view.id];
                        }
                    }
                    const auto view=imported_view_ids->find(row.target_id);
                    if (view==imported_view_ids->end() || view->second!=1)
                        document_error(DocumentErrorCode::invalid_entity,"Phase import output view requires one fresh sheet/view companion: "+id);
                } else if (!imported.contains(row.target_id))
                    document_error(DocumentErrorCode::invalid_entity,"Phase import annotation target must be fresh: "+id);
            }
        }
    }
    if (actual_registries!=registries)
        document_error(DocumentErrorCode::invalid_entity,"Phase import registry inventory must match all fresh registries");
    if (actual_reviewed_hierarchy!=reviewed_hierarchy)
        document_error(DocumentErrorCode::invalid_entity,"Reviewed phase import hierarchy inventory must match every reused roster owner");
    // Existing detached overlays must stay detached when fresh owners arrive.
    // Import cannot change their stored coordinates or infer a new attachment.
    for (const auto& [id,entity]:source.entities) {
        (void)id;
        if (entity.type!=kSheetViewEntityType) continue;
        const auto model=decode_sheet_view_entity(entity);
        for (const auto& view:model.views()) for (const auto& overlay:view.overlays)
            if (!overlay.object_id.empty() && !source.entities.contains(overlay.object_id) &&
                candidate.entities.contains(overlay.object_id))
                document_error(DocumentErrorCode::invalid_entity,"Phase import would attach an existing unresolved overlay: "+overlay.object_id);
    }
    // This resolves all saved alternatives, exclusive rosters and actual
    // physical host participation. Metadata alone cannot grant the policy.
    (void)constraint_phase_scope(candidate.entities);
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

std::string sha256_hex_stream(const std::function<void(const Sha256Sink&)>& produce) {
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
        std::array<unsigned char, 64 * 1024> staged{};
        std::size_t staged_size = 0;
        const auto flush = [&] {
            if (staged_size != 0) {
                check(BCryptHashData(hash, staged.data(), static_cast<ULONG>(staged_size), 0), "update");
                staged_size = 0;
            }
        };
        const Sha256Sink sink = [&](std::span<const std::byte> bytes) {
            while (!bytes.empty()) {
                const auto count = std::min(bytes.size(), staged.size() - staged_size);
                std::copy_n(reinterpret_cast<const unsigned char*>(bytes.data()), count,
                    staged.data() + staged_size);
                staged_size += count;
                bytes = bytes.subspan(count);
                if (staged_size == staged.size()) flush();
            }
        };
        produce(sink);
        flush();
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
    (void)produce;
    throw std::runtime_error("SHA-256 requires Windows BCrypt in this build");
#endif
}

std::string sha256_hex(std::span<const std::byte> bytes) {
    return sha256_hex_stream([bytes](const Sha256Sink& sink) { sink(bytes); });
}

bool is_known_entity_type(std::string_view type) noexcept {
    static constexpr std::array<std::string_view, 38> known{
        "property",             "building", "floor",  "layer", "boundary",
        "measurement_boundary", "room_boundary", "wall", "opening", "corner_window", "room",
        "slab",                 "roof",     "stair",  "railing", "column", "beam",
        "label",                "sheet",    "view",   "constraint", "dimension",
        "sheet_view_model",    "annotation_state", "reference_asset",
        "assembly_model", "assembly_instance", "model_phases", "room_relationships", "vertical_levels",
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

static std::string disto_completed_owner_id(const ApplyBoundaryConstraintChanges& geometry,
    const DistoMeasurementAttachment& attachment);
static void attach_disto_measurement(const std::map<std::string,Entity,std::less<>>& source,
    std::map<std::string,Entity,std::less<>>& completed,const DistoMeasurementAttachment& attachment,
    std::string_view completed_owner_id={});

void validate_physical_room_source_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    const BoundaryGeometryEdit* reviewed_edit=nullptr,
    const ApplyBoundaryConstraintChanges* reviewed_batch=nullptr,
    bool active_phase_constraints=false,
    bool composed_selection=false) {
    if (reviewed_batch && reviewed_batch->phase_constraint_authoring_completion &&
        !reviewed_batch->phase_constraint_authoring_intent.is_null()) {
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
        const auto intent=decode_phase_constraint_authoring_intent(reviewed_batch->phase_constraint_authoring_intent);
        const auto components = phase_constraint_replacement_components(intent);
        if (std::any_of(components.begin(), components.end(), [](const auto& component) {
                return !component.wall_replacement.is_null();
            })) {
            (void)command_to_json(Command{*reviewed_batch});
            auto replay=replay_phase_constraint_authoring(before,reviewed_batch->phase_constraint_authoring_intent);
            const bool ordinary_suffix=composed_selection || reviewed_batch->selection_completion;
            if (!ordinary_suffix && reviewed_batch->disto_measurement_completion) {
                if (!reviewed_batch->disto_measurement)
                    document_error(DocumentErrorCode::invalid_entity,"Proposed wall observation is missing");
                attach_disto_measurement(before,replay,*reviewed_batch->disto_measurement,
                    disto_completed_owner_id(*reviewed_batch,*reviewed_batch->disto_measurement));
            }
            if (!ordinary_suffix && entity_map_digest(replay)!=entity_map_digest(after))
                document_error(DocumentErrorCode::invalid_entity,"Proposed wall/room changes differ from their verified source-bound edit");
            if (ordinary_suffix) {
                // The child was compared exactly before the separately
                // admitted ordinary selection lane was merged. Its physical
                // room topology and source lineage still remain authoritative;
                // ordinary object/annotation edits cannot grant room authority.
                std::set<std::string,std::less<>> rooms;
                const auto& room_replay=std::as_const(replay);
                for (const auto* entities:{&room_replay,&after})
                    for (const auto& [id,entity]:*entities) if (is_physical_wall_room(entity)) rooms.insert(id);
                for (const auto& id:rooms) {
                    const auto original=room_replay.find(id),completed=after.find(id);
                    if (original==room_replay.end() || completed==after.end() ||
                        !is_physical_wall_room(original->second) || !is_physical_wall_room(completed->second) ||
                        original->second.extensions.at("physical_wall_room").dump()!=completed->second.extensions.at("physical_wall_room").dump() ||
                        decode_identified_boundary_entity(original->second)!=decode_identified_boundary_entity(completed->second))
                        document_error(DocumentErrorCode::invalid_entity,"Selection completion changed reviewed room topology or source lineage");
                }
            }
            return;
        }
#endif
    }
    // Explicit phase review is a separate authority. Admit a source-bound
    // proposal change only when its exclusive, canonical proof independently
    // reconstructs this complete destination; ordinary payloads cannot borrow it.
    if (reviewed_batch && (reviewed_batch->phase_room_review_completion ||
        !reviewed_batch->phase_room_review_intent.is_null())) {
        try {
            (void)command_to_json(Command{*reviewed_batch});
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
            const auto replay=replay_physical_wall_phase_room_review(before,reviewed_batch->phase_room_review_intent,active_phase_constraints);
            if (entity_map_digest(replay.entities)!=entity_map_digest(after))
                document_error(DocumentErrorCode::invalid_entity,
                    "Phase room changes differ from their independently verified destination");
            return;
#else
            document_error(DocumentErrorCode::invalid_entity,
                "Phase room changes require the production physical-wall engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    // The dedicated batch replay reconstructs the complete result from its
    // semantic decisions, including all retained owners, before publication.
    if (reviewed_batch && reviewed_batch->room_review_completion && !reviewed_batch->room_review_intent.is_null()) return;
    // The exclusive merge replay likewise reconstructs every affected room
    // from both original physical walls. Generic payloads cannot enter it.
    if (reviewed_batch && reviewed_batch->wall_merge) return;
    // Room-aware splits admit descriptor changes only under their complete,
    // independently reconstructed exclusive command result.
    if (reviewed_batch && reviewed_batch->wall_split &&
        reviewed_batch->wall_split->physical_room_completion) return;
    if (reviewed_edit && reviewed_edit->physical_wall_room_repair) {
        const auto descriptor=validate_physical_wall_room_repair(before,*reviewed_edit);
        const auto replacement=after.find(reviewed_edit->boundary_id);
        if (replacement==after.end() || !is_physical_wall_room(replacement->second) ||
            replacement->second.extensions.at("physical_wall_room")!=encode_physical_wall_room_descriptor(descriptor) ||
            replacement->second.properties.at("segments")!=reviewed_edit->replacement_segments)
            document_error(DocumentErrorCode::invalid_entity,"Physical room repair differs from its independently verified destination");
    }
    for (const auto& [id, entity] : before) {
        if (!is_physical_wall_room(entity)) continue;
        if (entity.extensions.at("physical_wall_room").at("version") != 1) continue;
        const auto replacement = after.find(id);
        if (replacement == after.end()) continue;
        if (reviewed_edit && reviewed_edit->physical_wall_room_repair && reviewed_edit->boundary_id==id) continue;
        if (!is_physical_wall_room(replacement->second) ||
            entity.extensions.at("physical_wall_room") !=
                replacement->second.extensions.at("physical_wall_room"))
            document_error(DocumentErrorCode::invalid_entity,
                "Physical room source evidence cannot be removed or replaced by a generic edit");
        if (decode_identified_boundary_entity(entity) !=
            decode_identified_boundary_entity(replacement->second))
            document_error(DocumentErrorCode::invalid_entity,
                "Edit a source-bound room through its physical walls; its independent outline is read-only");
    }
}

static bool has_supplemental_source_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.supplemental_source_completion || command.supplemental_asset_reference_completion ||
        !command.supplemental_entity_changes.empty() ||
        !command.supplemental_asset_changes.empty();
}

static bool has_selection_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.selection_completion || !command.selection_entity_changes.empty();
}

static ApplyBoundaryConstraintChanges without_selection_completion(ApplyBoundaryConstraintChanges command) {
    command.selection_completion = false;
    command.selection_entity_changes.clear();
    return command;
}

static bool has_independent_drawing_removal(const ApplyBoundaryConstraintChanges& command) {
    return command.independent_drawing_removal_completion || !command.independent_drawing_removal_intent.is_null();
}

static ApplyBoundaryConstraintChanges without_independent_drawing_removal(ApplyBoundaryConstraintChanges command) {
    command.independent_drawing_removal_completion=false;
    command.independent_drawing_removal_intent=nullptr;
    return command;
}

static void validate_independent_drawing_removal_mode(const ApplyBoundaryConstraintChanges& command) {
    const bool phase_authoring=command.phase_constraint_authoring_completion &&
        !command.phase_constraint_authoring_intent.is_null();
    if (!command.independent_drawing_removal_completion || command.independent_drawing_removal_intent.is_null() ||
        (!(command.room_review_completion && command.room_review_geometry_completion) &&
            !command.phase_room_review_completion && !phase_authoring))
        throw std::invalid_argument("Independent drawing removal requires a complete wall/room review or active design operation and its semantic intent");
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
    const auto intent=decode_drawing_selection_removal_intent(command.independent_drawing_removal_intent);
    if (encode_drawing_selection_removal_intent(intent).dump()!=command.independent_drawing_removal_intent.dump())
        throw std::invalid_argument("Independent drawing removal must retain its canonical semantic intent");
#else
    throw std::invalid_argument("Independent drawing removal requires the production authoring engine");
#endif
}

static bool exact_entity_payload(const Entity& left, const Entity& right);

// Shared with ordinary ApplyEntityChanges admission. Composition does not lend
// typed geometry authority to raw payloads in this lane.
static std::map<std::string, Entity, std::less<>> ordinary_entity_changes(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<EntityChange>& changes) {
    auto result = source;
    std::unordered_set<std::string> touched;
    for (const auto& change : changes) {
        if (change.kind != EntityChangeKind::upsert && change.kind != EntityChangeKind::erase)
            document_error(DocumentErrorCode::invalid_entity, "Invalid entity change kind");
        const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id;
        if (!touched.insert(id).second)
            document_error(DocumentErrorCode::duplicate_change, "entity is changed more than once in one command: " + id);
        if (change.kind == EntityChangeKind::upsert) {
            validate_entity(change.entity);
            result.insert_or_assign(id, change.entity);
        } else {
            if (!is_valid_identifier(id)) document_error(DocumentErrorCode::invalid_entity, "deleted entity id is invalid");
            result.erase(id);
        }
    }
    return result;
}

static bool supported_selection_entity(const Entity& entity) {
    const auto& p = entity.properties;
    const auto canonical = [&](int version, std::string_view form) {
        return p.contains("version") && p.at("version").is_number_integer() && p.at("version") == version &&
            p.contains("form") && p.at("form") == form;
    };
    if (entity.type == "column") return canonical(1,"rectangular_column") || canonical(1,"circular_column");
    if (entity.type == "beam") return canonical(1,"straight_beam");
    if (entity.type == "roof") return canonical(1,"sloped_roof_panel") || canonical(2,"sloped_roof_panel") ||
        canonical(1,"gable_roof") || canonical(2,"gable_roof") || canonical(1,"hip_roof") || canonical(2,"hip_roof");
    if (entity.type == "stair") return canonical(1,"straight_stair_flight") ||
        canonical(2,"multi_flight_stair") || canonical(3,"multi_flight_stair") || canonical(4,"multi_flight_stair");
    if (entity.type == "railing") return canonical(1,"straight_railing") ||
        canonical(2,"stair_flight_railing") || canonical(3,"stair_landing_railing");
    if (entity.type == "dimension") return decode_boundary_dimension_entity(entity).supported();
    if (entity.type == kAnnotationEntityType) { validate_annotation_entity(entity); return true; }
    if (entity.type == "assembly_instance") { (void)decode_document_assembly_instance(entity); return true; }
    if (entity.type == "assembly_model") { (void)AssemblyModel::from_json(p.at("model")); return true; }
    if (entity.type == "terrain_surface") { (void)TerrainSurface::from_json(p.at("model")); return true; }
    if (entity.type == "reference_asset") {
        const auto& position = p.at("position_m");
        return position.is_array() && position.size() == 2 && position[0].is_number() && position[1].is_number() &&
            std::isfinite(position[0].get<double>()) && std::isfinite(position[1].get<double>());
    }
    if (entity.type == "slab") return p.contains("boundary");
    if (entity.type == "room") return !is_physical_wall_room(entity) && (p.contains("boundary") || p.contains("segments"));
    if (entity.type == "opening") return p.contains("wall_id");
    if (entity.type == "corner_window") { (void)parse_corner_window(entity); return true; }
    if (entity.type == "roof_join") { (void)parse_roof_join(p, entity.id); return true; }
    return false;
}

static void validate_selection_changes(
    const std::map<std::string, Entity, std::less<>>& source,
    const ApplyBoundaryConstraintChanges& command) {
    if (!command.selection_completion || command.selection_entity_changes.size() > 1000)
        throw std::invalid_argument("Selection completion requires its marker and bounded ordinary lane");
    std::unordered_set<std::string> touched;
    for (const auto& change : command.selection_entity_changes) {
        if (change.kind != EntityChangeKind::upsert || !touched.insert(change.entity.id).second)
            throw std::invalid_argument("Selection completion requires unique existing-object upserts");
        const auto found = source.find(change.entity.id);
        if (found == source.end() || found->second.type != change.entity.type || found->second.required != change.entity.required ||
            !supported_selection_entity(found->second) || !supported_selection_entity(change.entity))
            throw std::invalid_argument("Selection completion requires existing same-type supported nonwall objects: " + change.entity.id);
        if (change.entity.type == "assembly_model") {
            auto retained = change.entity;
            const auto& before = found->second.properties.at("model").at("instances");
            auto& after = retained.properties.at("model").at("instances");
            if (before.size() != after.size()) throw std::invalid_argument("Selection cannot add or remove embedded assembly instances");
            for (std::size_t index = 0; index < before.size(); ++index) {
                if (before[index].at("id") != after[index].at("id") ||
                    before[index].contains("placement") != after[index].contains("placement") ||
                    (before[index].contains("placement") && before[index].at("placement") != after[index].at("placement")))
                    throw std::invalid_argument("Selection must retain embedded assembly identities and host bindings");
                if (before[index].contains("root_transform")) after[index]["root_transform"] = before[index].at("root_transform");
                else after[index].erase("root_transform");
            }
            if (!exact_entity_payload(found->second, retained))
                throw std::invalid_argument("Selection catalog completion may change only existing instance root transforms");
        }
    }
}

static bool has_disto_measurement_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.disto_measurement_completion || command.disto_measurement.has_value();
}

static ApplyBoundaryConstraintChanges without_disto_measurement(ApplyBoundaryConstraintChanges command) {
    command.disto_measurement.reset();
    command.disto_measurement_completion = false;
    return command;
}

static void restore_disto_measurements(const Entity& source, Entity& completed) {
    const auto prior = source.extensions.find("disto_measurements");
    if (prior == source.extensions.end()) completed.extensions.erase("disto_measurements");
    else completed.extensions["disto_measurements"] = *prior;
}

static std::vector<nlohmann::json> phase_constraint_authoring_proofs(const ApplyBoundaryConstraintChanges& command);
static std::string disto_completed_owner_id(const ApplyBoundaryConstraintChanges& geometry,
    const DistoMeasurementAttachment& attachment) {
    auto target=attachment.owner_id;
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
    for (const auto& encoded:phase_constraint_authoring_proofs(geometry)) {
        const auto intent=decode_phase_constraint_authoring_intent(encoded);
        if (intent.wall_replacement.is_null()) continue;
        const auto replacement=decode_phase_wall_replacement_authoring(intent.wall_replacement);
        const auto found=replacement.identities.find(attachment.owner_id);
        if (found==replacement.identities.end()) continue;
        if (target!=attachment.owner_id && target!=found->second)
            throw std::invalid_argument("DISTO observation has conflicting proposed owner mappings");
        target=found->second;
    }
#endif
    return target;
}

static void attach_disto_measurement(
    const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& completed,
    const DistoMeasurementAttachment& attachment,
    std::string_view completed_owner_id) {
    const auto encoded = nlohmann::json::parse(disto_measurement_json(attachment.record));
    if (!is_valid_identifier(attachment.owner_id))
        throw std::invalid_argument("DISTO observation owner ID is invalid");
    const auto prior_owner = source.find(attachment.owner_id);
    const auto completed_id=completed_owner_id.empty()?attachment.owner_id:std::string(completed_owner_id);
    const auto owner = completed.find(completed_id);
    if (prior_owner == source.end() || owner == completed.end() ||
        prior_owner->second.type != owner->second.type)
        throw std::invalid_argument("DISTO observation requires its admitted original or proposed completed owner");
    auto& entity = owner->second;
    const auto& record = attachment.record;
    const auto target = record.target_field;
    const auto compatible = [&](std::string_view type, std::string_view field) {
        return entity.type == type && target == field;
    };
    const bool wall_length = compatible("wall", "wall.length");
    const char* property = nullptr;
    if (compatible("opening", "opening.width")) property = "width_m";
    else if (compatible("wall", "wall.height") || compatible("opening", "opening.height") ||
             compatible("room", "room.height")) property = "height_m";
    else if (compatible("wall", "wall.thickness") || compatible("slab", "slab.thickness")) property = "thickness_m";
    else if (compatible("wall", "wall.elevation") || compatible("slab", "slab.elevation") ||
             compatible("room", "room.elevation")) property = "elevation_m";
    if (!wall_length && !property)
        throw std::invalid_argument("DISTO observation field is incompatible with its owner");

    std::array<char, 128> number{};
    const auto converted = std::to_chars(number.data(), number.data() + number.size(), record.value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{})
        throw std::invalid_argument("DISTO observation value cannot be represented");
    const auto quantity = parse_quantity(std::string(number.data(), converted.ptr) + " " + record.unit, Unit::metre);
    if (!std::isfinite(quantity.metres) || quantity.metres <= 1e-7)
        throw std::invalid_argument("DISTO observation is below the supported measurement range");
    double actual{};
    double representation_tolerance{};
    if (wall_length) {
        const auto& axis = entity.properties.at("baseline");
        const auto& start = axis.at("start");
        const auto& end = axis.at("end");
        if (!start.is_array() || start.size() != 2 || !end.is_array() || end.size() != 2 ||
            !start[0].is_number() || !start[1].is_number() || !end[0].is_number() || !end[1].is_number())
            throw std::invalid_argument("DISTO wall observation requires a valid completed axis");
        const Segment baseline{{start[0].get<double>(), start[1].get<double>()},
            {end[0].get<double>(), end[1].get<double>()}, axis.value("sweep_radians", 0.0)};
        actual = segment_length(baseline);
        // Account only for endpoint subtraction and curved-axis double arithmetic.
        // A solver tolerance or display precision cannot qualify a different value.
        const double chord = std::hypot(baseline.end.x - baseline.start.x, baseline.end.y - baseline.start.y);
        const double curve_factor = chord > 0.0 ? std::max(1.0, actual / chord) : 1.0;
        representation_tolerance = 16.0 * std::numeric_limits<double>::epsilon() * curve_factor *
            std::max({1.0, std::abs(quantity.metres), std::abs(baseline.start.x), std::abs(baseline.start.y),
                std::abs(baseline.end.x), std::abs(baseline.end.y)});
    } else {
        if (!entity.properties.contains(property) || !entity.properties.at(property).is_number())
            throw std::invalid_argument("DISTO observation requires a numeric completed field");
        actual = entity.properties.at(property).get<double>();
    }
    if (!std::isfinite(actual) || (wall_length &&
        (!std::isfinite(representation_tolerance) || representation_tolerance >= std::min(actual, quantity.metres))) || (wall_length
        ? std::abs(actual - quantity.metres) > representation_tolerance
        : actual != quantity.metres))
        throw std::invalid_argument("DISTO observation differs from the completed geometry field");

    const auto prior = prior_owner->second.extensions.find("disto_measurements");
    const auto next = entity.extensions.find("disto_measurements");
    if ((prior == prior_owner->second.extensions.end()) != (next == entity.extensions.end()) ||
        (prior != prior_owner->second.extensions.end() && *prior != *next))
        throw std::invalid_argument("Geometry proof cannot alter DISTO observations before attachment");
    if (prior != prior_owner->second.extensions.end()) {
        if (!prior->is_object() || !prior->contains("version") || !prior->at("version").is_number_integer() ||
            prior->at("version") != 1 || !prior->contains("fields") || !prior->at("fields").is_object())
            throw std::invalid_argument("Existing DISTO observation metadata is malformed or unsupported");
        if (prior->at("fields").contains(target)) {
            if (parse_disto_measurement_json(prior->at("fields").at(target).dump()).target_field != target)
                throw std::invalid_argument("Existing DISTO observation field identity is inconsistent");
            if (!attachment.replace_existing)
                throw std::invalid_argument("DISTO observation replacement requires explicit authorization");
        }
    } else entity.extensions["disto_measurements"] = {{"version", 1}, {"fields", nlohmann::json::object()}};
    entity.extensions.at("disto_measurements").at("fields")[target] = encoded;
}

static bool has_rigid_wall_transform(const ApplyBoundaryConstraintChanges& command) {
    return command.rigid_wall_transform_completion || std::any_of(command.wall_edits.begin(),command.wall_edits.end(),
        [](const auto& edit){return edit.version==4 || edit.version==5;});
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

static bool has_rigid_group_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.rigid_group_completion || command.rigid_group_transform.has_value();
}

static bool has_joint_translation_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.joint_translation_completion || command.joint_translation.has_value();
}

static bool has_phase_room_review_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.phase_room_review_completion || !command.phase_room_review_intent.is_null();
}

static bool has_phase_constraint_authoring(const ApplyBoundaryConstraintChanges& command) {
    return command.phase_constraint_authoring_completion || !command.phase_constraint_authoring_intent.is_null();
}

static bool has_room_review_geometry_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.room_review_geometry_completion || !command.room_review_geometry_proof.is_null();
}

static bool has_room_review_batch_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.room_review_batch_completion || !command.room_review_additional_intents.empty();
}

static std::vector<nlohmann::json> room_review_intents(const ApplyBoundaryConstraintChanges& command) {
    std::vector<nlohmann::json> result;
    result.reserve(1+command.room_review_additional_intents.size());
    result.push_back(command.room_review_intent);
    result.insert(result.end(),command.room_review_additional_intents.begin(),command.room_review_additional_intents.end());
    return result;
}

static bool has_room_review_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.room_review_completion || !command.room_review_intent.is_null() ||
        has_room_review_geometry_completion(command) || has_room_review_batch_completion(command);
}

static bool room_review_context_selection(const nlohmann::json& intent) {
    if (intent.is_null()) return false;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    return decode_physical_wall_room_review_intent(intent).context_plane_selection;
#else
    return intent.is_object() && (intent.value("version",0)==2 ||
        (intent.value("version",0)==3 && intent.value("context_plane_selection",false)));
#endif
}

static void validate_phase_room_review_mode(const ApplyBoundaryConstraintChanges& command) {
    if (!has_phase_room_review_completion(command)) return;
    if (!command.phase_room_review_completion || command.phase_room_review_intent.is_null())
        throw std::invalid_argument("Phase room review requires its explicit mode and complete intent");
    if (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.entity_changes.empty() ||
        !command.physical_entity_changes.empty() || !command.exterior_source_edits.empty() ||
        !command.supplemental_entity_changes.empty() || !command.supplemental_asset_changes.empty() ||
        !command.measured_stroke_edits.empty() || !command.dimension_placement_moves.empty() ||
        !command.selection_entity_changes.empty() || command.selection_completion ||
        command.exterior_source_completion || command.supplemental_source_completion ||
        command.supplemental_asset_reference_completion || command.rigid_wall_transform_completion ||
        command.measured_source_completion || command.dimension_placement_completion ||
        command.rigid_group_completion || command.rigid_group_transform || command.wall_split || command.wall_merge ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc ||
        has_joint_translation_completion(command) || has_room_review_completion(command) ||
        command.wall_dimension_completion || command.curve_construction_completion || has_disto_measurement_completion(command) ||
        has_phase_constraint_authoring(command))
        throw std::invalid_argument("Phase room review cannot borrow another edit or asset authority");
}

static void validate_phase_constraint_authoring_mode(const ApplyBoundaryConstraintChanges& command) {
    if (!command.phase_constraint_authoring_completion || command.phase_constraint_authoring_intent.is_null())
        throw std::invalid_argument("Active design authoring requires its retained mode and semantic intent");
    if (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.entity_changes.empty() ||
        !command.physical_entity_changes.empty() || !command.exterior_source_edits.empty() ||
        !command.supplemental_entity_changes.empty() || !command.supplemental_asset_changes.empty() ||
        !command.measured_stroke_edits.empty() || !command.dimension_placement_moves.empty() ||
        !command.selection_entity_changes.empty() || command.selection_completion ||
        command.exterior_source_completion || command.supplemental_source_completion ||
        command.supplemental_asset_reference_completion || command.rigid_wall_transform_completion ||
        command.measured_source_completion || command.dimension_placement_completion ||
        command.rigid_group_completion || command.rigid_group_transform || command.wall_split || command.wall_merge ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc ||
        has_joint_translation_completion(command) || has_room_review_completion(command) ||
        has_phase_room_review_completion(command) || command.wall_dimension_completion ||
        command.curve_construction_completion || has_disto_measurement_completion(command))
        throw std::invalid_argument("Active design authoring cannot borrow raw geometry, assets or another edit authority");
}

static void validate_room_aware_wall_split_mode(const ApplyBoundaryConstraintChanges& command) {
    if (!command.wall_split || (!command.wall_split->physical_room_completion &&
        command.wall_split->physical_room_owners.empty())) return;
    if (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.entity_changes.empty() ||
        !command.physical_entity_changes.empty() || !command.exterior_source_edits.empty() ||
        !command.supplemental_entity_changes.empty() || !command.supplemental_asset_changes.empty() ||
        !command.measured_stroke_edits.empty() || !command.dimension_placement_moves.empty() ||
        command.exterior_source_completion || command.supplemental_source_completion ||
        command.supplemental_asset_reference_completion || command.rigid_wall_transform_completion ||
        command.measured_source_completion || command.dimension_placement_completion ||
        command.rigid_group_completion || command.rigid_group_transform || command.wall_merge ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc ||
        has_joint_translation_completion(command) || has_room_review_completion(command) ||
        has_disto_measurement_completion(command))
        throw std::invalid_argument("Room-aware wall split intent cannot borrow another command lane");
}

static void validate_wall_merge_mode(const ApplyBoundaryConstraintChanges& command) {
    if (!command.wall_merge) return;
    if (!is_valid_identifier(command.wall_merge->first_wall_id) ||
        !is_valid_identifier(command.wall_merge->second_wall_id) ||
        command.wall_merge->first_wall_id == command.wall_merge->second_wall_id)
        throw std::invalid_argument("Wall merge requires two distinct valid wall identities");
    if (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.entity_changes.empty() ||
        !command.physical_entity_changes.empty() || !command.exterior_source_edits.empty() ||
        !command.supplemental_entity_changes.empty() || !command.supplemental_asset_changes.empty() ||
        !command.measured_stroke_edits.empty() || !command.dimension_placement_moves.empty() ||
        command.exterior_source_completion || command.supplemental_source_completion ||
        command.supplemental_asset_reference_completion || command.rigid_wall_transform_completion ||
        command.measured_source_completion || command.dimension_placement_completion ||
        command.rigid_group_completion || command.rigid_group_transform || command.wall_split ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc ||
        has_joint_translation_completion(command) || has_room_review_completion(command) ||
        has_disto_measurement_completion(command))
        throw std::invalid_argument("Wall merge intent cannot borrow another command lane");
}

static void validate_room_review_mode(const ApplyBoundaryConstraintChanges& command, bool admission) {
    if (!has_room_review_completion(command)) return;
    if (!command.room_review_completion || (admission && command.room_review_intent.is_null()))
        throw std::invalid_argument("Room review requires its explicit semantic intent and completion mode");
    if (has_room_review_batch_completion(command) &&
        (!command.room_review_batch_completion || !command.room_review_geometry_completion ||
            command.room_review_geometry_proof.is_null() || command.room_review_intent.is_null() ||
            command.room_review_additional_intents.empty() || command.room_review_additional_intents.size()>31 ||
            has_selection_completion(command)))
        throw std::invalid_argument("Room review batch requires one original geometry proof and two to thirty-two explicit decisions");
    if (!command.boundary_edits.empty() || !command.wall_edits.empty() || !command.entity_changes.empty() ||
        !command.physical_entity_changes.empty() || !command.exterior_source_edits.empty() ||
        !command.supplemental_entity_changes.empty() || !command.supplemental_asset_changes.empty() ||
        !command.measured_stroke_edits.empty() || !command.dimension_placement_moves.empty() ||
        command.exterior_source_completion || command.supplemental_source_completion ||
        command.supplemental_asset_reference_completion || command.rigid_wall_transform_completion ||
        command.measured_source_completion || command.dimension_placement_completion ||
        command.rigid_group_completion || command.rigid_group_transform || command.wall_split || command.wall_merge ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc ||
        has_joint_translation_completion(command) || command.wall_dimension_completion ||
        command.curve_construction_completion || has_disto_measurement_completion(command))
        throw std::invalid_argument("Room review cannot borrow another command's edit authority");
}

static void validate_joint_translation_mode(const ApplyBoundaryConstraintChanges& command, bool admission) {
    if (!has_joint_translation_completion(command)) return;
    if (!command.joint_translation_completion)
        throw std::invalid_argument("Joint translation requires its explicit completion mode");
    if (command.rigid_group_completion || command.rigid_group_transform || command.wall_split || command.wall_merge ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc)
        throw std::invalid_argument("Joint translation cannot borrow an independent edit authority");
    if (admission && !command.joint_translation)
        throw std::invalid_argument("Joint translation requires its selected-source intent");
}

static bool joint_per_target_presentation(const JointTranslationIntent& intent) {
    return intent.per_owner_rigid_completion || !intent.owner_transformations.empty() ||
        intent.per_owner_translation_completion || !intent.owner_translations.empty() ||
        !intent.dimension_translations.empty() || intent.per_target_presentation_completion || !intent.annotation_translations.empty() ||
        !intent.reference_translations.empty();
}

// These complete raw owners are redundant proofs, never an edit authority.
// Admission compares every byte of their entity payload with source replay.
static void validate_joint_presentation_proof(const ApplyBoundaryConstraintChanges& command) {
    if (!command.joint_translation || !joint_per_target_presentation(*command.joint_translation)) return;
    if (!command.supplemental_asset_changes.empty() || command.supplemental_asset_reference_completion ||
        command.supplemental_entity_changes.size() > 1000 ||
        (!command.supplemental_entity_changes.empty() && !command.supplemental_source_completion))
        throw std::invalid_argument("Joint presentation proof cannot carry assets or unbounded owners");
    std::map<std::string, std::string, std::less<>> targets;
    for (const auto& target : command.joint_translation->annotation_translations)
        targets.emplace(target.owner_id, kAnnotationEntityType);
    for (const auto& target : command.joint_translation->reference_translations)
        if (!targets.emplace(target.reference_id, "reference_asset").second)
            throw std::invalid_argument("Joint reference proof aliases another presentation owner");
    std::set<std::string, std::less<>> touched;
    for (const auto& change : command.supplemental_entity_changes) {
        const auto target = targets.find(change.entity.id);
        const bool derived_rigid_annotation = (command.joint_translation->per_owner_rigid_completion ||
            !command.joint_translation->owner_transformations.empty()) &&
            change.entity.type == kAnnotationEntityType;
        if (change.kind != EntityChangeKind::upsert || (!derived_rigid_annotation && (target == targets.end() ||
            change.entity.type != target->second)) || !touched.insert(change.entity.id).second)
            throw std::invalid_argument("Joint presentation proof requires unique selected annotation/reference upserts");
    }
}

static void remove_joint_presentation_proof(ApplyBoundaryConstraintChanges& command) {
    command.supplemental_entity_changes.clear();
    // Presentation rows no longer need ordinary supplemental admission. Retain
    // the flag if physical metadata still needs that existing geometry lane.
    command.supplemental_source_completion = command.supplemental_source_completion &&
        !command.physical_entity_changes.empty();
}

static void validate_rigid_group_intent(const ApplyBoundaryConstraintChanges& command, bool admission) {
    if (!has_rigid_group_completion(command)) return;
    if (!command.rigid_group_completion)
        throw std::invalid_argument("Rigid group lane requires its explicit completion mode");
    if (command.wall_split || command.wall_merge || command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc)
        throw std::invalid_argument("Rigid group composition cannot borrow split, corner, resize or arc authority");
    if (command.rigid_group_transform && command.rigid_group_transform->expected_revision != command.expected_revision)
        throw std::invalid_argument("Rigid group child must use the parent's expected revision");
    if (admission && (!command.rigid_group_transform || command.wall_edits.empty()))
        throw std::invalid_argument("Rigid group composition requires a rigid child and connected wall proof");
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
    if (command.wall_split || command.wall_merge || command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc)
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

static void validate_wall_dimension_completion(const ApplyBoundaryConstraintChanges& command) {
    if (!command.wall_dimension_completion) return;
    if (command.wall_edits.empty() || !command.boundary_edits.empty() || !command.physical_entity_changes.empty() ||
        command.wall_split || command.wall_merge || command.exterior_corner_move || command.exterior_segment_resize ||
        command.exterior_segment_arc || has_dimension_placement_completion(command) || has_rigid_group_completion(command) ||
        has_joint_translation_completion(command) || has_room_review_completion(command) || has_disto_measurement_completion(command))
        throw std::invalid_argument("Wall callout completion requires only qualified rigid wall and measured-stroke geometry");
    const auto& shared = command.wall_edits.front().rigid_transform;
    if (!shared) throw std::invalid_argument("Wall callout completion requires an explicit rigid source transform");
    for (const auto& edit : command.wall_edits)
        if ((edit.version != 4 && edit.version != 5) || !edit.rigid_transform || !(*edit.rigid_transform == *shared))
            throw std::invalid_argument("Wall callout completion requires one shared qualified rigid wall transform");
    for (const auto& edit : command.measured_stroke_edits)
        if (!edit.rigid_transform || !(*edit.rigid_transform == *shared) || edit.authored_edit || !edit.vertex_edits.empty())
            throw std::invalid_argument("Wall callout completion cannot borrow a different or partial measured-stroke transform");
}

static bool has_curve_construction_completion(const ApplyBoundaryConstraintChanges& command) {
    return command.curve_construction_completion ||
        std::any_of(command.wall_edits.begin(),command.wall_edits.end(),[](const auto& edit) {
            return edit.version == 6 || edit.curve_construction.has_value() || edit.wall_classification.has_value();
        });
}

static void validate_curve_construction_completion(const ApplyBoundaryConstraintChanges& command) {
    const bool constructed = std::any_of(command.wall_edits.begin(),command.wall_edits.end(),[](const auto& edit) {
        return edit.version == 6 || edit.curve_construction.has_value() || edit.wall_classification.has_value();
    });
    if (!command.curve_construction_completion && !constructed) return;
    if (!command.curve_construction_completion || !constructed)
        throw std::invalid_argument("Curve construction requires its retained mode and explicit wall proof");
    if (command.wall_split || command.wall_merge || command.exterior_corner_move || command.exterior_segment_resize ||
        command.exterior_segment_arc || has_rigid_group_completion(command) || has_joint_translation_completion(command) ||
        has_room_review_completion(command) || command.wall_dimension_completion || has_rigid_wall_transform(command) ||
        has_dimension_placement_completion(command) || has_disto_measurement_completion(command))
        throw std::invalid_argument("Curve construction cannot borrow another geometric intent");
    if (!command.physical_entity_changes.empty() || has_supplemental_source_completion(command) ||
        command.supplemental_asset_reference_completion)
        throw std::invalid_argument("Curve construction cannot borrow raw physical, supplemental or asset edits");
    for (const auto& edit : command.wall_edits)
        if ((edit.curve_construction || edit.wall_classification) && edit.version != 6)
            throw std::invalid_argument("Curve construction fields require wall proof version six");
}

static bool ordinary_room_wall_proof_version(int version) {
    return version==2 || version==3 || version==4 || version==5 ||
        version==6 || version==7 || version==11;
}

static Command room_review_geometry_command(const ApplyBoundaryConstraintChanges& command) {
    validate_room_review_mode(command,true);
    if (!command.room_review_geometry_completion || command.room_review_geometry_proof.is_null() ||
        has_selection_completion(command))
        throw std::invalid_argument("Wall room review requires its explicit geometry mode and proof");
    const auto& proof=command.room_review_geometry_proof;
    if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
        !proof.contains("kind") || (proof.at("kind")!="apply_boundary_constraint_changes" &&
            proof.at("kind")!="apply_entity_changes" && proof.at("kind")!="physical_wall_deletion" && proof.at("kind")!="mixed_wall_deletion" &&
            proof.at("kind")!="mixed_wall_opening_deletion") ||
        proof.dump().size()>1024*1024)
        throw std::invalid_argument("Room review requires one bounded direct physical-wall proof");
    const auto version=proof.at("version").get<int>();
    const bool grouped_deletion=proof.at("kind")=="physical_wall_deletion";
    const bool mixed_deletion=proof.at("kind")=="mixed_wall_deletion";
    const bool mixed_opening_deletion=proof.at("kind")=="mixed_wall_opening_deletion";
    if ((mixed_opening_deletion && version!=40) || (mixed_deletion && version!=37 && version!=39) ||
        (grouped_deletion && version!=31 && version!=35 && version!=36 && version!=38) || (!grouped_deletion && !mixed_deletion && !mixed_opening_deletion &&
        version!=1 && version!=10 && version!=17 && version!=19 && version!=21 && version!=22 && version!=23 && version!=34 && !ordinary_room_wall_proof_version(version)))
        throw std::invalid_argument("Room review cannot wrap another geometry intent");
    const auto decoded=[&]()->Command {
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
        if (grouped_deletion) return decode_physical_wall_deletion_review_proof(proof);
        if (mixed_deletion) return Command{decode_mixed_wall_deletion_review_proof(proof).command};
        if (mixed_opening_deletion) return Command{decode_mixed_wall_opening_deletion_review_proof(proof).command};
#else
        if (grouped_deletion || mixed_deletion || mixed_opening_deletion) throw std::invalid_argument("Physical wall deletion review is unavailable");
#endif
        return command_from_json(proof);
    }();
    const auto* geometry=std::get_if<ApplyBoundaryConstraintChanges>(&decoded);
    const auto* ordinary=std::get_if<ApplyEntityChanges>(&decoded);
    if ((!geometry && !ordinary) ||
        (geometry && (geometry->expected_revision!=command.expected_revision || geometry->message!=command.message)) ||
        (ordinary && (ordinary->expected_revision!=command.expected_revision || ordinary->message!=command.message)))
        throw std::invalid_argument("Wall room review must retain the original geometry command identity");
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    if (is_physical_wall_room_selection_geometry_review_command(decoded)) {
        if (command_to_json(decoded).dump()!=proof.dump())
            throw std::invalid_argument("Room review mixed selection must retain its complete canonical wall geometry proof");
        return decoded;
    }
    if (is_physical_wall_room_deletion_review_command(decoded)) {
        for (const auto& encoded:room_review_intents(command))
            if (!decode_physical_wall_room_review_intent(encoded).context_plane_selection)
                throw std::invalid_argument("Wall deletion requires explicit context and plane room review");
        if (command_to_json(decoded)!=((grouped_deletion || mixed_deletion || mixed_opening_deletion) ? proof.at("proof") : proof))
            throw std::invalid_argument("Room review wall deletion proof must be canonical");
        return decoded;
    }
    if (is_physical_wall_room_profile_review_command(decoded)) {
        if (command_to_json(decoded)!=proof)
            throw std::invalid_argument("Room review physical-wall profile proof must be canonical");
        return decoded;
    }
    if (is_physical_wall_room_rigid_review_command(decoded)) {
        if (command_to_json(decoded)!=proof)
            throw std::invalid_argument("Room review rigid physical-wall proof must be canonical");
        return decoded;
    }
    if (is_physical_wall_room_joint_review_command(decoded)) {
        if (command_to_json(decoded)!=proof)
            throw std::invalid_argument("Room review joint physical-wall proof must retain its intact canonical intent");
        return decoded;
    }
    if (is_physical_wall_room_active_constraint_review_command(decoded)) {
        if (command_to_json(decoded)!=proof)
            throw std::invalid_argument("Room review must retain its intact active design proof");
        return decoded;
    }
#endif
    if (!geometry || proof.at("kind")!="apply_boundary_constraint_changes")
        throw std::invalid_argument("Room review ordinary payload must be one supported wall profile edit");
    if (version==34) {
        validate_phase_constraint_authoring_mode(*geometry);
        if (command_to_json(decoded)!=proof)
            throw std::invalid_argument("Room review must retain the canonical active design intent");
    } else if (version==23) {
        if (!geometry->curve_construction_completion)
            throw std::invalid_argument("Curve room review requires explicit curve authority");
        validate_curve_construction_completion(*geometry);
    } else {
        if (geometry->wall_edits.empty() ||
            std::any_of(geometry->wall_edits.begin(),geometry->wall_edits.end(),[](const auto& edit) {
                return edit.version<1 || edit.version>3 || edit.rigid_transform || edit.curve_construction || edit.wall_classification;
            }) || geometry->wall_split || geometry->wall_merge || geometry->exterior_corner_move ||
            geometry->exterior_segment_resize || geometry->exterior_segment_arc ||
            has_rigid_group_completion(*geometry) || has_joint_translation_completion(*geometry) ||
            has_room_review_completion(*geometry) || has_selection_completion(*geometry) ||
            has_curve_construction_completion(*geometry) || geometry->wall_dimension_completion ||
            has_rigid_wall_transform(*geometry) || has_dimension_placement_completion(*geometry) ||
            has_disto_measurement_completion(*geometry) || geometry->supplemental_asset_reference_completion ||
            !geometry->supplemental_asset_changes.empty())
            throw std::invalid_argument("Room review requires ordinary physical-wall edits and their admitted source consequences");
        if (command_to_json(decoded)!=proof)
            throw std::invalid_argument("Room review physical-wall proof must retain its canonical ordinary dialect");
    }
    return decoded;
}

static int room_review_geometry_dialect(const ApplyBoundaryConstraintChanges& completion,const Command& geometry) {
    (void)completion;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    if (is_physical_wall_room_deletion_review_command(geometry))
        return (completion.room_review_geometry_proof.at("kind")=="physical_wall_deletion" ||
            completion.room_review_geometry_proof.at("kind")=="mixed_wall_deletion" ||
            completion.room_review_geometry_proof.at("kind")=="mixed_wall_opening_deletion")
            ? completion.room_review_geometry_proof.at("version").get<int>() : 30;
    if (is_physical_wall_room_rigid_review_command(geometry)) return 28;
    if (is_physical_wall_room_joint_review_command(geometry)) return 32;
#endif
    const auto* constrained=std::get_if<ApplyBoundaryConstraintChanges>(&geometry);
    if (constrained && has_phase_constraint_authoring(*constrained)) return 32;
    if (!constrained || constrained->wall_edits.empty()) return 26;
    return constrained->curve_construction_completion ? 24 : 25;
}

static std::vector<nlohmann::json> phase_constraint_authoring_proofs(const ApplyBoundaryConstraintChanges& command) {
    // Only strict, canonical command envelopes supply policy or source authority.
    // Opaque metadata with similarly named fields never participates.
    const auto encoded=command_to_json(Command{command});
    std::vector<nlohmann::json> result;
    const auto visit=[&](const auto& self,const nlohmann::json& proof,unsigned depth)->void {
        if (depth>2) throw std::invalid_argument("Active design proof wrapper depth is invalid");
        if (proof.at("kind")!="apply_boundary_constraint_changes") return;
        const auto version=proof.at("version").get<int>();
        if (version==34) result.push_back(proof.at("phase_constraint_authoring_intent"));
        // The new independent removal is one validated outer enclosure;
        // preserve the historical child walk's existing depth allowance.
        else if (version==41 || version==42) self(self,proof.at("proof"),depth);
        else if (version==22 || version==19) self(self,proof.at("proof"),depth+1);
        else if (proof.contains("room_review_geometry_proof"))
            self(self,proof.at("room_review_geometry_proof"),depth+1);
    };
    visit(visit,encoded,0);
    return result;
}

#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
static std::vector<PhaseConstraintAuthoringIntent> phase_constraint_authoring_components(
    const ApplyBoundaryConstraintChanges& command) {
    std::vector<PhaseConstraintAuthoringIntent> result;
    for (const auto& encoded : phase_constraint_authoring_proofs(command)) {
        auto components = phase_constraint_replacement_components(decode_phase_constraint_authoring_intent(encoded));
        for (auto& component : components) result.push_back(std::move(component));
    }
    return result;
}
#endif

static bool phase_constraint_authoring_preserves_registries(const ApplyBoundaryConstraintChanges& command) {
    const auto proofs=phase_constraint_authoring_proofs(command);
    return std::none_of(proofs.begin(),proofs.end(),[](const auto& intent) {
        return (intent.contains("wall_replacement") && !intent.at("wall_replacement").is_null()) ||
            (intent.contains("opening_demolition") && !intent.at("opening_demolition").is_null()) ||
            (intent.contains("roof_replacement") && !intent.at("roof_replacement").is_null()) ||
            (intent.contains("slab_replacement") && !intent.at("slab_replacement").is_null()) ||
            (intent.contains("slab_demolition") && !intent.at("slab_demolition").is_null()) ||
            (intent.contains("coordinated_replacements") && !intent.at("coordinated_replacements").is_null()) ||
            (intent.contains("structural_replacement") && !intent.at("structural_replacement").is_null()) ||
            (intent.contains("stair_demolition") && !intent.at("stair_demolition").is_null()) ||
            (intent.contains("stair_replacement") && !intent.at("stair_replacement").is_null()) ||
            (intent.contains("stair_demolition_retirement") && !intent.at("stair_demolition_retirement").is_null()) ||
            (intent.contains("coordinated_demolition") && !intent.at("coordinated_demolition").is_null()) ||
            (intent.contains("wall_demolition") && !intent.at("wall_demolition").is_null());
    });
}
static bool phase_constraint_authoring_retires_proposals(const ApplyBoundaryConstraintChanges& command) {
    const auto proofs=phase_constraint_authoring_proofs(command);
    return std::any_of(proofs.begin(), proofs.end(), [](const auto& intent) {
        // The closed complete wall-removal replay admits active reference
        // retirement, including reviewed room consequences. Inactive rows
        // remain protected by their exact raw subsequence below.
        if (intent.contains("wall_demolition") && !intent.at("wall_demolition").is_null()) return true;
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
        const auto replacement_retires=[](const nlohmann::json& value) {
            if (!value.is_object() || value.value("version",0)!=5) return false;
            const auto replacement=decode_phase_stair_replacement_authoring(value);
            return std::any_of(replacement.dependency_dispositions.begin(),replacement.dependency_dispositions.end(),
                [](const auto& decision) { return decision.action==PhaseStairReplacementDependencyAction::retire; });
        };
        const auto replacement=intent.find("stair_replacement");
        if (replacement!=intent.end() && replacement_retires(*replacement)) return true;
        const auto replacements=intent.find("coordinated_replacements");
        if (replacements!=intent.end() && replacements->is_object()) {
            const auto stair=replacements->find("stair_replacement");
            if (stair!=replacements->end() && replacement_retires(*stair)) return true;
        }
#endif
        if (intent.contains("stair_demolition_retirement") && !intent.at("stair_demolition_retirement").is_null())
            return true;
        const auto coordinated=intent.find("coordinated_demolition");
        if (coordinated==intent.end() || !coordinated->is_object()) return false;
        if (coordinated->value("version",0)==5 || coordinated->value("version",0)==6 ||
            coordinated->value("version",0)==7) return true;
        if ((coordinated->value("version",0)==2 || coordinated->value("version",0)==3 ||
             coordinated->value("version",0)==4) &&
            coordinated->contains("ordinary_removal") &&
            coordinated->at("ordinary_removal").is_object()) return true;
        const auto stair=coordinated->find("stair_authoring");
        return stair!=coordinated->end() && stair->is_object() &&
            stair->contains("stair_demolition_retirement") && !stair->at("stair_demolition_retirement").is_null();
    });
}
static void validate_phase_constraint_composed_originals(const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate,const ApplyBoundaryConstraintChanges& command) {
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
    for (const auto& encoded:phase_constraint_authoring_proofs(command)) {
        const auto intent=decode_phase_constraint_authoring_intent(encoded);
        if (!intent.coordinated_replacements.is_null() || !intent.coordinated_demolition.is_null() ||
            !intent.wall_demolition.is_null() || !intent.ordinary_roof_edits.is_null()) {
            const auto replay = replay_phase_constraint_authoring(source, encoded);
            if (entity_map_digest(replay) != entity_map_digest(candidate))
                throw std::invalid_argument("Coordinated architectural authoring differs from its actual source replay");
            continue;
        }
        if (!intent.wall_replacement.is_null()) validate_phase_wall_replacement_originals(source,candidate,intent);
        if (!intent.opening_demolition.is_null()) {
            const auto replay = replay_phase_opening_demolition_entities(source,
                decode_phase_opening_demolition_intent(intent.opening_demolition));
            if (entity_map_digest(replay) != entity_map_digest(candidate))
                throw std::invalid_argument("Opening demolition cannot change retained owners or borrow other edit authority");
        }
        if (!intent.roof_replacement.is_null()) {
            const auto replay = replay_phase_roof_replacement_authoring(source,
                decode_phase_roof_replacement_authoring(intent.roof_replacement));
            if (entity_map_digest(replay) != entity_map_digest(candidate))
                throw std::invalid_argument("Roof replacement cannot change retained owners or borrow other edit authority");
        }
        if (!intent.slab_replacement.is_null()) {
            const auto replay = replay_phase_slab_replacement_authoring(source,
                decode_phase_slab_replacement_authoring(intent.slab_replacement));
            if (entity_map_digest(replay) != entity_map_digest(candidate))
                throw std::invalid_argument("Slab replacement cannot change retained owners or borrow other edit authority");
        }
        if (!intent.slab_demolition.is_null()) {
            const auto replay = replay_phase_slab_demolition_entities(source,
                decode_slab_demolition_intent(intent.slab_demolition));
            if (entity_map_digest(replay) != entity_map_digest(candidate))
                throw std::invalid_argument("Slab demolition cannot change retained owners or borrow other edit authority");
        }
        if (!intent.structural_replacement.is_null()) {
            const auto replay=replay_phase_structural_replacement_authoring(source,
                decode_phase_structural_replacement_authoring(intent.structural_replacement));
            if (entity_map_digest(replay)!=entity_map_digest(candidate))
                throw std::invalid_argument("Structural replacement cannot change retained owners or borrow other edit authority");
        }
        if (!intent.stair_demolition.is_null()) {
            const auto replay=replay_phase_stair_demolition_entities(source,
                decode_stair_demolition_intent(intent.stair_demolition));
            if (entity_map_digest(replay)!=entity_map_digest(candidate))
                throw std::invalid_argument("Stair demolition cannot change retained owners or borrow other edit authority");
        }
        if (!intent.stair_replacement.is_null()) {
            const auto replay=replay_phase_stair_replacement_authoring(source,
                decode_phase_stair_replacement_authoring(intent.stair_replacement));
            if (entity_map_digest(replay)!=entity_map_digest(candidate))
                throw std::invalid_argument("Stair replacement differs from its actual source profile replay");
        }
        if (!intent.stair_demolition_retirement.is_null()) {
            const auto replay=replay_phase_stair_demolition_retirement_entities(source,
                decode_stair_demolition_retirement_intent(intent.stair_demolition_retirement));
            if (entity_map_digest(replay)!=entity_map_digest(candidate))
                throw std::invalid_argument("Stair dependent retirement differs from its actual source replay");
        }
    }
#endif
}

static void validate_active_design_preserved_dependents(const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate,bool freeze_registries=true,
    bool retires_proposals=false) {
    const auto scope=constraint_phase_scope(source);
    std::set<std::string,std::less<>> inactive_targets=scope.inactive_owner_ids;
    for (const auto& id : scope.inactive_owner_ids) {
        const auto& entity=source.at(id);
        if (can_recognize_boundary_entity_type(entity.type) &&
            inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1)
            for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
                inactive_targets.insert(edge.segment_id);
                inactive_targets.insert(edge.start_vertex_id);
                inactive_targets.insert(edge.end_vertex_id);
            }
    }
    const auto require_exact=[&](const std::string& id,const Entity& original) {
        const auto after=candidate.find(id);
        if (after==candidate.end() || !exact_entity_payload(original,after->second))
            throw std::invalid_argument("Active design edit changed a preserved dependent: "+id);
    };
    if (freeze_registries)
        for (const auto& registry : scope.registries) require_exact(registry.registry_id,source.at(registry.registry_id));
    for (const auto& [id,original] : source) {
        if (scope.inactive_owner_ids.contains(id)) { require_exact(id,original);continue; }
        if (original.type=="constraint") {
            const auto decoded=decode_constraint_entity(original);
            if (decoded.supported() && !constraint_participates(*decoded.constraint,scope)) require_exact(id,original);
        } else if (can_recognize_boundary_dimension_entity_type(original.type)) {
            const auto decoded=decode_boundary_dimension_entity(original);
            if (decoded.supported() && inactive_targets.contains(decoded.dimension->boundary_id)) require_exact(id,original);
        } else if (original.type=="opening") {
            std::string host,diagnostic;
            if (!read_document_wall_id(original,host,diagnostic)) throw std::invalid_argument(diagnostic);
            if (scope.inactive_owner_ids.contains(host)) require_exact(id,original);
        } else if (original.type==kAnnotationEntityType) {
            validate_annotation_entity(original);
            const auto& rows=original.properties.at("state").at("overrides");
            if (retires_proposals) {
                // Only exact actual-source typed retirement may remove active
                // rows. Protected inactive rows keep their raw values, order
                // and multiplicity even when earlier active rows disappear.
                const auto after=candidate.find(id);
                if (after==candidate.end() || after->second.type!=original.type)
                    throw std::invalid_argument("Active design retirement removed preserved annotations");
                validate_annotation_entity(after->second);
                const auto protected_rows=[&](const nlohmann::json& records) {
                    auto retained=nlohmann::json::array();
                    for (const auto& row:records)
                        if (inactive_targets.contains(row.at("target_id").get<std::string>())) retained.push_back(row);
                    return retained;
                };
                if (protected_rows(rows).dump()!=protected_rows(after->second.properties.at("state").at("overrides")).dump())
                    throw std::invalid_argument("Active design retirement changed an inactive annotation placement");
                continue;
            }
            for (std::size_t index=0;index<rows.size();++index) {
                const auto& row=rows.at(index);
                if (!inactive_targets.contains(row.at("target_id").get<std::string>())) continue;
                const auto after=candidate.find(id);
                if (after==candidate.end() || after->second.type!=original.type)
                    throw std::invalid_argument("Active design edit removed preserved annotations");
                validate_annotation_entity(after->second);
                const auto& retained=after->second.properties.at("state").at("overrides");
                if (index>=retained.size() || row.dump()!=retained.at(index).dump())
                    throw std::invalid_argument("Active design edit changed an inactive annotation placement");
            }
        }
    }
}

static std::vector<bool> active_constraint_history_policies(const std::vector<RevisionRecord>& history) {
    std::vector<bool> result(history.size(),false);
    for (std::size_t index=0;index<history.size();++index) {
        const auto& record=history[index];
        if (record.revision!=index)
            document_error(DocumentErrorCode::invalid_history,"Constraint policy history is not contiguous");
        if (index==0) {
            if (record.source_revision || record.boundary_constraint_changes || record.phase_entity_import)
                document_error(DocumentErrorCode::invalid_history,"Create record cannot supply constraint policy");
            continue;
        }
        if (record.source_revision) {
            if (*record.source_revision>=index || record.boundary_constraint_changes || record.phase_entity_import ||
                (record.action!="undo" && record.action!="redo"))
                document_error(DocumentErrorCode::invalid_history,"Constraint policy navigation is invalid");
            result[index]=result[static_cast<std::size_t>(*record.source_revision)];
        } else {
            result[index]=result[index-1];
            if (record.phase_entity_import) {
                if (record.name || record.phase_entity_import->expected_revision!=index-1)
                    document_error(DocumentErrorCode::invalid_history,"Phase import constraint policy source is invalid");
                result[index]=true;
            }
            if (record.boundary_constraint_changes) {
                try {
                    result[index]=result[index] || !phase_constraint_authoring_proofs(*record.boundary_constraint_changes).empty();
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history,error.what());
                }
            }
        }
    }
    return result;
}

#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
static void validate_phase_constraint_authoring_source(const DocumentSnapshot& source,
    const ApplyBoundaryConstraintChanges& command) {
    for (const auto& encoded : phase_constraint_authoring_proofs(command)) {
        const auto intent=decode_phase_constraint_authoring_intent(encoded);
        if (intent.expected_revision!=source.revision() ||
            intent.source_snapshot_digest!=document_snapshot_digest(source) ||
            intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
            intent.source_saved_revision!=source.saved_revision_optional() ||
            intent.source_entities_digest!=entity_map_digest(source.entities()) ||
            intent.phase_selections!=phase_constraint_authoring_selections(source.entities()))
            document_error(DocumentErrorCode::stale_revision,"Active design authoring source snapshot changed");
        if (!intent.wall_demolition.is_null()) {
            const auto demolition=decode_phase_wall_demolition_authoring(intent.wall_demolition);
            validate_physical_wall_join_removal_identity_lifetime(source,demolition.wall_additional_identities);
        }
    }
}

static void validate_retained_phase_constraint_authoring_source(const DocumentSnapshot& snapshot,
    const RevisionRecord& source, const ApplyBoundaryConstraintChanges& command,std::size_t preceding_records) {
    for (const auto& encoded : phase_constraint_authoring_proofs(command)) {
        const auto intent=decode_phase_constraint_authoring_intent(encoded);
        if (intent.expected_revision!=source.revision ||
            intent.source_snapshot_digest!=document_snapshot_digest_at_revision(snapshot,source.revision,intent.source_saved_revision) ||
            intent.source_authoring_digest!=document_authoring_source_digest_v2_at_revision(snapshot,source.revision) ||
            intent.source_entities_digest!=entity_map_digest(source.entities) ||
            intent.phase_selections!=phase_constraint_authoring_selections(source.entities))
            throw std::invalid_argument("Active design authoring retained source authority changed");
        if (!intent.wall_demolition.is_null()) {
            const auto demolition=decode_phase_wall_demolition_authoring(intent.wall_demolition);
            validate_physical_wall_join_removal_identity_lifetime(source.entities,snapshot.history(),preceding_records,
                demolition.wall_additional_identities);
        }
    }
}

#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
static void validate_phase_room_review_lifetime(const nlohmann::json& encoded,
    const std::vector<RevisionRecord>& history,std::size_t preceding_records);
#endif
static bool has_complete_wall_join_deletion_proof(const ApplyBoundaryConstraintChanges& command) {
    const auto& proof=command.room_review_geometry_proof;
    return command.room_review_geometry_completion && proof.is_object() &&
        proof.contains("kind") && proof.contains("version") && proof.at("version").is_number_integer() &&
        ((proof.at("kind")=="physical_wall_deletion" && (proof.at("version")==36 || proof.at("version")==38)) ||
         (proof.at("kind")=="mixed_wall_deletion" && (proof.at("version")==37 || proof.at("version")==39)) ||
         (proof.at("kind")=="mixed_wall_opening_deletion" && proof.at("version")==40));
}
static PhysicalWallJoinRemovalAdditionalIdentities complete_wall_join_deletion_destinations(
    const ApplyBoundaryConstraintChanges& command) {
    if (!has_complete_wall_join_deletion_proof(command)) return {};
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    if (command.room_review_geometry_proof.at("kind")=="mixed_wall_opening_deletion") {
        const auto decoded=decode_mixed_wall_opening_deletion_review_proof(command.room_review_geometry_proof);
        auto result=decoded.intent.wall_additional_identities;
        for (const auto& [owner,ids]:decoded.intent.other.roof_additional_identities)
            if (!result.emplace(owner,ids).second)
                throw std::invalid_argument("Mixed wall/opening and roof split destination owners overlap");
        return result;
    }
    if (command.room_review_geometry_proof.at("kind")=="mixed_wall_deletion") {
        const auto decoded=decode_mixed_wall_deletion_review_proof(command.room_review_geometry_proof);
        auto result=decoded.intent.wall_additional_identities;
        for (const auto& [owner,ids]:decoded.intent.other.roof_additional_identities)
            if (!result.emplace(owner,ids).second)
                throw std::invalid_argument("Mixed wall and roof split destination owners overlap");
        return result;
    }
    (void)decode_physical_wall_deletion_review_proof(command.room_review_geometry_proof);
#else
    throw std::invalid_argument("Complete wall-join removal review is unavailable");
#endif
    return command.room_review_geometry_proof.at("additional_join_identities").get<PhysicalWallJoinRemovalAdditionalIdentities>();
}
static std::set<std::string,std::less<>> phase_demolition_ordinary_roof_destinations(
    const PhaseConstraintAuthoringIntent& intent) {
    std::set<std::string,std::less<>> result;
    const auto reserve=[&](const auto& mappings) {
        for (const auto& [original,ids]:mappings) {
            (void)original;
            for (const auto& id:ids)
                if (!result.insert(id).second)
                    throw std::invalid_argument("Architectural removal repeats a declared roof destination: "+id);
        }
    };
    auto historical=intent;
    if (!intent.wall_demolition.is_null()) {
        const auto complete=decode_phase_wall_demolition_authoring(intent.wall_demolition);
        reserve(complete.ordinary.roof_additional_identities);
        if (complete.other_authoring.is_null()) return result;
        historical=decode_phase_constraint_authoring_intent(complete.other_authoring);
    }
    if (!historical.coordinated_demolition.is_null())
        if (const auto ordinary=phase_coordinated_demolition_ordinary_removal(historical.coordinated_demolition,historical);
            ordinary && ordinary->version==2) reserve(ordinary->roof_additional_identities);
    return result;
}

static void validate_phase_constraint_fresh_lifetime(const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate,
    const std::vector<RevisionRecord>& history,std::size_t preceding_records,
    const ApplyBoundaryConstraintChanges& command) {
    std::set<std::string,std::less<>> fresh;
    std::set<std::string,std::less<>> nested_fresh;
    const bool phase_drawing_enclosure=has_independent_drawing_removal(command) && has_phase_constraint_authoring(command);
    bool complete_envelope_reservation=phase_drawing_enclosure;
    bool wall_stack_asset_reservation=false;
    bool hosted_slab_asset_reservation=false;
    bool roof_mixed_asset_reservation=false;
    bool wall_presentation_asset_reservation=false;
    bool structural_asset_reservation=phase_drawing_enclosure;
    bool structural_hosted_alias_reservation=phase_drawing_enclosure;
    const bool wall_join_asset_reservation=has_complete_wall_join_deletion_proof(command);
    std::set<std::pair<std::string,std::string>> proposed_hosted_instances;
    std::set<std::string,std::less<>> ordinary_roof_destinations;
    std::set<std::string,std::less<>> wall_demolition_room_destinations;
    std::set<std::string,std::less<>> wall_demolition_join_destinations;
    bool complete_wall_demolition=false;
    bool complete_ordinary_wall_demolition=false;
    bool complete_wall_catalog_demolition=false;
    bool complete_hosted_demolition=false;
    bool complete_opening_demolition=false;
    for (const auto& [id,entity] : candidate) {
        (void)entity;
        if (!source.contains(id)) fresh.insert(id);
    }
    if (wall_join_asset_reservation) {
        complete_envelope_reservation=true;
        structural_hosted_alias_reservation=true;
        const auto join_additional=complete_wall_join_deletion_destinations(command);
        for (const auto& [original,ids]:join_additional) {
            (void)original;
            for (const auto& id:ids) {
                if (!nested_fresh.insert(id).second)
                    throw std::invalid_argument("A wall-join split repeats a declared destination: "+id);
                fresh.insert(id);
            }
        }
        validate_physical_wall_join_removal_identity_lifetime(source,history,preceding_records,join_additional);
    }
    for (const auto& encoded:phase_constraint_authoring_proofs(command)) {
        const auto root_intent=decode_phase_constraint_authoring_intent(encoded);
        for (const auto& id:phase_demolition_ordinary_roof_destinations(root_intent))
            if (!ordinary_roof_destinations.insert(id).second)
                throw std::invalid_argument("A complete removal repeats a declared roof destination: "+id);
        if (!root_intent.wall_demolition.is_null()) {
            complete_wall_demolition=true;
            complete_envelope_reservation=true;
            roof_mixed_asset_reservation=true;
            structural_asset_reservation=true;
            structural_hosted_alias_reservation=true;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
            const auto demolition=decode_phase_wall_demolition_authoring(root_intent.wall_demolition);
            complete_wall_catalog_demolition=complete_wall_catalog_demolition || demolition.complete_hosted_catalog_consequences;
            complete_ordinary_wall_demolition=complete_ordinary_wall_demolition ||
                !demolition.ordinary_wall_ids.empty() || demolition.complete_hosted_catalog_consequences;
            for (const auto& [original,ids]:demolition.wall_additional_identities) {
                (void)original;
                for (const auto& id:ids) {
                    if (!wall_demolition_join_destinations.insert(id).second)
                        throw std::invalid_argument("Wall demolition repeats a declared join destination: "+id);
                    fresh.insert(id);
                }
            }
            if (!demolition.room_review_intent.is_null()) {
                validate_phase_room_review_lifetime(demolition.room_review_intent,history,preceding_records);
                const auto rooms=decode_physical_wall_phase_room_review_intent(demolition.room_review_intent);
                const auto reserve_room=[&](const std::string& id) {
                    if (!wall_demolition_room_destinations.insert(id).second)
                        throw std::invalid_argument("Wall demolition room destinations overlap: "+id);
                    fresh.insert(id);
                };
                for (const auto& plane:rooms.planes) {
                    for (const auto& decision:plane.fresh) {
                        if (decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::create_proposed &&
                            decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) continue;
                        if (decision.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) reserve_room(decision.room_id);
                        for (const auto& id:decision.fresh_ids.segment_ids) reserve_room(id);
                        for (const auto& id:decision.fresh_ids.vertex_ids) reserve_room(id);
                    }
                    for (const auto& decision:plane.source_rooms)
                        for (const auto& id:decision.replacement_dimension_ids) reserve_room(id);
                }
            }
#endif
        }
        if (!root_intent.coordinated_demolition.is_null()) {
            complete_envelope_reservation=true;
            if (phase_coordinated_demolition_complete_hosted_catalog_consequences(
                    root_intent.coordinated_demolition,root_intent)) {
                complete_hosted_demolition=true;
                complete_opening_demolition=complete_opening_demolition ||
                    root_intent.coordinated_demolition.value("version",0)==5 ||
                    root_intent.coordinated_demolition.value("version",0)==6 ||
                    root_intent.coordinated_demolition.value("version",0)==7;
                structural_asset_reservation=true;
                structural_hosted_alias_reservation=true;
            }
            // Historical roof demolition children can omit phase-qualified
            // joins. The new enclosure always reserves every destination
            // against retained assets, independently of the child's dialect.
            roof_mixed_asset_reservation=true;
        }
        if (!root_intent.coordinated_replacements.is_null()) {
            complete_envelope_reservation=true;
            roof_mixed_asset_reservation=true;
            std::vector<RoofEditIntent> ordinary_roof_edits;
            for (const auto& row : root_intent.coordinated_replacements.at("ordinary_roof_edits"))
                ordinary_roof_edits.push_back(decode_roof_edit_intent(row));
            for (const auto& id : new_roof_opening_identity_ids(source, roof_edit_opening_intents(ordinary_roof_edits))) {
                if (!fresh.insert(id).second)
                    throw std::invalid_argument("A coordinated roof opening overlaps another fresh identity: "+id);
                nested_fresh.insert(id);
            }
        }
        if (!root_intent.ordinary_roof_edits.is_null()) {
            complete_envelope_reservation=true;
            roof_mixed_asset_reservation=true;
            std::vector<RoofEditIntent> edits;
            for (const auto& row:root_intent.ordinary_roof_edits) edits.push_back(decode_roof_edit_intent(row));
            for (const auto& id:new_roof_opening_identity_ids(source,roof_edit_opening_intents(edits))) {
                if (!fresh.insert(id).second)
                    throw std::invalid_argument("An ordinary roof opening overlaps another fresh identity: "+id);
                nested_fresh.insert(id);
            }
        }
    }
    for (const auto& intent : phase_constraint_authoring_components(command)) {
        if (!intent.stair_replacement.is_null()) {
            const auto replacement=decode_phase_stair_replacement_authoring(intent.stair_replacement);
            complete_envelope_reservation=true;
            structural_asset_reservation=true;
            structural_hosted_alias_reservation=true;
            for (const auto& [original,id]:replacement.identities) {
                (void)original;fresh.insert(id);nested_fresh.insert(id);
            }
            const auto reserve_nested=[&](const auto& identities) {
                for (const auto& [original,id]:identities) {
                    (void)original;
                    if (!fresh.insert(id).second)
                        throw std::invalid_argument("A proposed stair child overlaps another fresh identity: "+id);
                    nested_fresh.insert(id);
                }
            };
            reserve_nested(replacement.child_identities);
            reserve_nested(replacement.hosted_instance_identities);
            reserve_nested(replacement.overlay_identities);
            for (const auto& [original,id]:replacement.hosted_instance_identities)
                proposed_hosted_instances.emplace(replacement.identities.at(original.first),id);
        }
        if (!intent.structural_replacement.is_null()) {
            const auto replacement=decode_phase_structural_replacement_authoring(intent.structural_replacement);
            complete_envelope_reservation=true;
            structural_asset_reservation=true;
            structural_hosted_alias_reservation=structural_hosted_alias_reservation || replacement.complete_hosted;
            for (const auto& [original,id]:replacement.identities) {
                (void)original;fresh.insert(id);nested_fresh.insert(id);
            }
            for (const auto& [original,id]:replacement.hosted_instance_identities) {
                if (!fresh.insert(id).second)
                    throw std::invalid_argument("A proposed structural component overlaps another fresh identity: "+id);
                nested_fresh.insert(id);
                proposed_hosted_instances.emplace(replacement.identities.at(original.first),id);
            }
        }
        if (!intent.roof_replacement.is_null()) {
            const auto replacement=decode_phase_roof_replacement_authoring(intent.roof_replacement);
            roof_mixed_asset_reservation=roof_mixed_asset_reservation ||
                !replacement.ordinary_roof_edits.empty() || replacement.phase_qualified_joins;
            complete_envelope_reservation=complete_envelope_reservation ||
                !replacement.roof_opening_edits.empty() || !replacement.roof_edits.empty() || replacement.demolition ||
                replacement.include_hosted_instances;
            hosted_slab_asset_reservation=hosted_slab_asset_reservation || replacement.include_hosted_instances;
            for (const auto& [original,id]:replacement.identities) {
                (void)original;fresh.insert(id);nested_fresh.insert(id);
            }
            for (const auto& [original,id]:replacement.hosted_instance_identities) {
                if (!fresh.insert(id).second)
                    throw std::invalid_argument("A proposed roof-hosted component overlaps another fresh identity: "+id);
                nested_fresh.insert(id);
                proposed_hosted_instances.emplace(replacement.identities.at(original.first),id);
            }
            for (const auto& [original, ids] : replacement.demolition_additional_identities) {
                (void)original;
                for (const auto& id : ids) { fresh.insert(id); nested_fresh.insert(id); }
            }
            auto roof_edits=replacement.roof_edits;
            roof_edits.insert(roof_edits.end(),replacement.ordinary_roof_edits.begin(),replacement.ordinary_roof_edits.end());
            const auto opening_edits=roof_edits.empty() ? replacement.roof_opening_edits :
                roof_edit_opening_intents(roof_edits);
            for (const auto& id:new_roof_opening_identity_ids(source,opening_edits)) {
                if (!fresh.insert(id).second)
                    throw std::invalid_argument("A new roof opening overlaps another fresh replacement identity: "+id);
                nested_fresh.insert(id);
            }
        }
        if (!intent.slab_replacement.is_null()) {
            const auto replacement=decode_phase_slab_replacement_authoring(intent.slab_replacement);
            complete_envelope_reservation=true;
            for (const auto& [original,id]:replacement.identities) {
                (void)original;fresh.insert(id);nested_fresh.insert(id);
            }
            hosted_slab_asset_reservation=hosted_slab_asset_reservation ||
                !replacement.hosted_instance_identities.empty() || replacement.coordinate_ordinary_hosted_geometry;
            for (const auto& [original,id]:replacement.hosted_instance_identities) {
                if (!fresh.insert(id).second)
                    throw std::invalid_argument("A proposed hosted component overlaps another fresh replacement identity: "+id);
                nested_fresh.insert(id);
                proposed_hosted_instances.emplace(replacement.identities.at(original.first),id);
            }
            for (const auto& stack : replacement.slab_stacks) {
                Slab original; std::string diagnostic;
                if (!read_document_slab(source.at(stack.slab_id), original, diagnostic))
                    throw std::invalid_argument(diagnostic);
                for (const auto& row : stack.layers) {
                    if (std::any_of(original.layers.begin(), original.layers.end(), [&](const auto& layer) {
                            return layer.id == row.layer_id;
                        })) continue;
                    if (!fresh.insert(row.layer_id).second)
                        throw std::invalid_argument("A new slab layer overlaps another fresh replacement identity: " + row.layer_id);
                    nested_fresh.insert(row.layer_id);
                }
            }
        }
        if (intent.wall_replacement.is_null()) continue;
        const auto replacement=decode_phase_wall_replacement_authoring(intent.wall_replacement);
        if (replacement.complete_presentations) {
            complete_envelope_reservation=true;
            wall_presentation_asset_reservation=true;
        }
        if (!replacement.wall_stacks.empty()) {
            complete_envelope_reservation=true;
            wall_stack_asset_reservation=true;
        }
        for (const auto& [original,id]:replacement.identities) {
            (void)original;fresh.insert(id);
            if (complete_envelope_reservation) nested_fresh.insert(id);
        }
        for (const auto& stack : replacement.wall_stacks) {
            const auto& original=source.at(stack.wall_id);
            std::set<std::string,std::less<>> existing;
            if (original.properties.contains("layers"))
                for (const auto& layer:original.properties.at("layers"))
                    existing.insert(layer.at("id").get<std::string>());
            for (const auto& row:stack.layers) if (!existing.contains(row.layer_id)) {
                if (!fresh.insert(row.layer_id).second)
                    throw std::invalid_argument("A new wall layer overlaps another fresh replacement identity: "+row.layer_id);
                nested_fresh.insert(row.layer_id);
            }
        }
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
        if (!replacement.room_review_intent.is_null()) {
            validate_phase_room_review_lifetime(replacement.room_review_intent,history,preceding_records);
            const auto room=decode_physical_wall_phase_room_review_intent(replacement.room_review_intent);
            std::set<std::string,std::less<>> mapped;
            for (const auto& [original,id]:replacement.identities) { (void)original;mapped.insert(id); }
            const auto reserve_room=[&](const std::string& id) {
                if (mapped.contains(id)) throw std::invalid_argument("Proposed room identity overlaps a declared wall copy: "+id);
                fresh.insert(id);
                if (complete_envelope_reservation) nested_fresh.insert(id);
            };
            for (const auto& plane:room.planes) {
                for (const auto& decision:plane.fresh) {
                    if (decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::create_proposed &&
                        decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) continue;
                    if (decision.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) reserve_room(decision.room_id);
                    for (const auto& id:decision.fresh_ids.segment_ids) reserve_room(id);
                    for (const auto& id:decision.fresh_ids.vertex_ids) reserve_room(id);
                }
                for (const auto& decision:plane.source_rooms)
                    for (const auto& id:decision.replacement_dimension_ids) reserve_room(id);
            }
        }
#endif
    }
    for (const auto& id:wall_demolition_join_destinations)
        if (!nested_fresh.insert(id).second)
            throw std::invalid_argument("An ordinary wall join destination overlaps another declared identity: "+id);
    for (const auto& id:wall_demolition_room_destinations)
        if (!nested_fresh.insert(id).second)
            throw std::invalid_argument("A wall demolition room destination overlaps another declared identity: "+id);
    for (const auto& id:ordinary_roof_destinations) {
        if (!nested_fresh.insert(id).second)
            throw std::invalid_argument("A coordinated ordinary roof split overlaps another fresh identity: "+id);
        fresh.insert(id);
    }
    if (!proposed_hosted_instances.empty()) {
        const auto presentations=embedded_assembly_presentation_ids(candidate);
        for (const auto& qualified:proposed_hosted_instances) {
            const auto& alias=presentations.at(qualified);
            if (!fresh.insert(alias).second)
                throw std::invalid_argument("A proposed component presentation overlaps another fresh identity: "+alias);
            nested_fresh.insert(alias);
        }
    }
    if (complete_wall_demolition) nested_fresh.insert(fresh.begin(),fresh.end());
    // Inner one predates this vocabulary. Its retained identities keep their
    // historical meaning; only inner two/three introduce these reserved fields.
    if (complete_ordinary_wall_demolition) {
        for (const auto* token:{"version","wall_demolition","other_authoring","ordinary","opening_ids",
            "room_review_intent","registry_id","alternative_id","wall_ids","ordinary_wall_ids",
            "wall_additional_identities","object_ids","components","catalog_id","instance_id",
            "roof_additional_identities","expected_revision","source_snapshot_digest","source_authoring_digest",
            "source_entities_digest","source_saved_revision","phase_selections","intent"})
            if (fresh.contains(token))
                throw std::invalid_argument("A fresh wall demolition destination borrows a semantic proof field: "+std::string(token));
    }
    if (complete_hosted_demolition) {
        nested_fresh.insert(fresh.begin(),fresh.end());
        for (const auto* token:{"version","coordinated_demolition","opening_authoring","roof_authoring",
            "slab_authoring","structural_authoring","stair_authoring","ordinary_removal",
            "complete_hosted_catalog_consequences","object_ids","components","catalog_id","instance_id",
            "roof_additional_identities","registry_id","alternative_id","expected_revision",
            "source_snapshot_digest","source_authoring_digest","source_entities_digest",
            "source_saved_revision","phase_selections","intent"})
            if (fresh.contains(token))
                throw std::invalid_argument("A fresh architectural demolition destination borrows a semantic proof field: "+std::string(token));
    }
    if (complete_wall_catalog_demolition && fresh.contains("complete_hosted_catalog_consequences"))
        throw std::invalid_argument("A fresh wall demolition destination borrows the complete catalog proof field");
    if (complete_opening_demolition && fresh.contains("ordinary_opening_ids"))
        throw std::invalid_argument("A fresh architectural demolition destination borrows the ordinary opening proof field");
    if (phase_drawing_enclosure) {
        // The complete new enclosure reserves every resulting or declared
        // destination against both stage vocabularies, even for older phase
        // children whose original dialect required less envelope reservation.
        nested_fresh.insert(fresh.begin(),fresh.end());
        for (const auto* token:{"independent_drawing_removal_completion","independent_drawing_removal_intent",
            "owner_ids","annotations","owner_id","child_id","proof"})
            if (fresh.contains(token))
                throw std::invalid_argument("A fresh phase destination borrows an independent drawing proof token: "+std::string(token));
    }
    if (fresh.empty()) return;
    if (fresh.size()>4096) throw std::invalid_argument("Active design fresh identity budget exceeded");
    const auto reserve_clone_sources=[&](const PhaseConstraintAuthoringIntent& root) {
        std::vector<RoofOpeningEditIntent> openings;
        const auto append=[&](const nlohmann::json& rows) {
            if (rows.is_null()) return;
            for (const auto& row:rows) {
                const auto edit=decode_roof_edit_intent(row);
                if (edit.openings) openings.push_back(*edit.openings);
            }
        };
        append(root.ordinary_roof_edits);
        if (!root.coordinated_replacements.is_null()) append(root.coordinated_replacements.at("ordinary_roof_edits"));
        for (const auto& leaf:phase_constraint_replacement_components(root)) if (!leaf.roof_replacement.is_null()) {
            const auto replacement=decode_phase_roof_replacement_authoring(leaf.roof_replacement);
            openings.insert(openings.end(),replacement.roof_opening_edits.begin(),replacement.roof_opening_edits.end());
            for (const auto* rows:{&replacement.roof_edits,&replacement.ordinary_roof_edits})
                for (const auto& edit:*rows) if (edit.openings) openings.push_back(*edit.openings);
        }
        std::size_t nodes=0,bytes=0;
        const auto scan=[&](const auto& self,const nlohmann::json& value,unsigned depth)->void {
            if (depth>64 || ++nodes>4*1024*1024)
                throw std::invalid_argument("Retained roof clone source exceeds the identity JSON budget");
            const auto text=[&](const std::string& name) {
                if (name.size()>64*1024*1024-bytes)
                    throw std::invalid_argument("Retained roof clone source exceeds the identity string budget");
                bytes+=name.size();
                if (fresh.contains(name))
                    throw std::invalid_argument("A fresh identity aliases a captured roof clone source: "+name);
            };
            if (value.is_string()) text(value.template get_ref<const std::string&>());
            else if (value.is_array()) for (const auto& child:value) self(self,child,depth+1);
            else if (value.is_object()) for (const auto& [name,child]:value.items()) {text(name);self(self,child,depth+1);}
        };
        for (const auto& edit:openings) for (const auto& upsert:edit.upserts) if (upsert.clone_source) {
            const auto& roof=upsert.clone_source->roof;
            scan(scan,nlohmann::json{{"id",roof.id},{"type",roof.type},{"properties",roof.properties},
                {"required",roof.required},{"extensions",roof.extensions}},0);
        }
    };
    for (const auto& encoded:phase_constraint_authoring_proofs(command))
        reserve_clone_sources(decode_phase_constraint_authoring_intent(encoded));
    if (complete_envelope_reservation)
        for (const auto* key:{"id","type","properties","required","extensions"})
            if (nested_fresh.contains(key))
                throw std::invalid_argument("Proposed identity aliases an entity envelope field: "+std::string(key));
    for (std::size_t index=0;index<preceding_records;++index) {
        const auto& record=history.at(index);
        if (wall_stack_asset_reservation || hosted_slab_asset_reservation || roof_mixed_asset_reservation ||
            wall_presentation_asset_reservation || structural_asset_reservation || wall_join_asset_reservation)
            for (const auto& [id,asset]:record.assets) {
                (void)asset;
                if (fresh.contains(id))
                    throw std::invalid_argument("Proposed identity collides with a retained asset: "+id);
            }
        // A deliberately omitted fresh relationship copy is still a declared
        // identity in the recorded semantic operation. Undo cannot release it.
        if (record.boundary_constraint_changes) {
            if (has_complete_wall_join_deletion_proof(*record.boundary_constraint_changes))
                for (const auto& [original,ids]:complete_wall_join_deletion_destinations(*record.boundary_constraint_changes)) {
                    (void)original;
                    for (const auto& id:ids) {
                        if (fresh.contains(id))
                            throw std::invalid_argument("Fresh identity was already reserved by retained wall-join split intent: "+id);
                    }
                }
            for (const auto& encoded:phase_constraint_authoring_proofs(*record.boundary_constraint_changes)) {
                const auto root=decode_phase_constraint_authoring_intent(encoded);
                reserve_clone_sources(root);
                const auto reserve_openings=[&](const nlohmann::json& rows) {
                    if (rows.is_null()) return;
                    for (const auto& row:rows) {
                        const auto edit=decode_roof_edit_intent(row);
                        if (edit.openings) for (const auto& upsert:edit.openings->upserts)
                            if (fresh.contains(upsert.opening_id))
                                throw std::invalid_argument("Roof opening identity was already reserved by retained typed intent: "+upsert.opening_id);
                    }
                };
                reserve_openings(root.ordinary_roof_edits);
                if (!root.coordinated_replacements.is_null())
                    reserve_openings(root.coordinated_replacements.at("ordinary_roof_edits"));
                for (const auto& id:phase_demolition_ordinary_roof_destinations(root))
                    if (fresh.contains(id))
                        throw std::invalid_argument("Proposed identity was already reserved by retained ordinary roof split intent: "+id);
                if (!root.wall_demolition.is_null()) {
                    const auto demolition=decode_phase_wall_demolition_authoring(root.wall_demolition);
                    for (const auto& [original,ids]:demolition.wall_additional_identities) {
                        (void)original;
                        for (const auto& id:ids) if (fresh.contains(id))
                            throw std::invalid_argument("Fresh identity was already reserved by retained ordinary wall split intent: "+id);
                    }
                }
            }
        }
        if (record.boundary_constraint_changes)
            for (const auto& intent : phase_constraint_authoring_components(*record.boundary_constraint_changes)) {
                const auto require_unused=[&](const auto& identities) {
                    for (const auto& [original,id]:identities) {
                        (void)original;
                        if (fresh.contains(id)) throw std::invalid_argument("Proposed identity was already reserved by retained replacement intent: "+id);
                    }
                };
                if (!intent.wall_replacement.is_null()) {
                    const auto wall=decode_phase_wall_replacement_authoring(intent.wall_replacement);
                    require_unused(wall.identities);
                    for (const auto& stack:wall.wall_stacks)
                        for (const auto& row:stack.layers) if (fresh.contains(row.layer_id))
                            throw std::invalid_argument("Wall layer identity was already reserved by retained intent: "+row.layer_id);
                }
                if (!intent.slab_replacement.is_null()) {
                    const auto slab = decode_phase_slab_replacement_authoring(intent.slab_replacement);
                    require_unused(slab.identities);
                    require_unused(slab.hosted_instance_identities);
                    for (const auto& stack : slab.slab_stacks)
                        for (const auto& row : stack.layers) if (fresh.contains(row.layer_id))
                            throw std::invalid_argument("Slab layer identity was already reserved by retained intent: " + row.layer_id);
                }
                if (!intent.roof_replacement.is_null()) {
                    const auto roof=decode_phase_roof_replacement_authoring(intent.roof_replacement);
                    require_unused(roof.identities);
                    require_unused(roof.hosted_instance_identities);
                    for (const auto& [original, ids] : roof.demolition_additional_identities) {
                        (void)original;
                        for (const auto& id : ids) if (fresh.contains(id))
                            throw std::invalid_argument("Proposed identity was already reserved by retained roof split intent: " + id);
                    }
                    auto roof_edits=roof.roof_edits;
                    roof_edits.insert(roof_edits.end(),roof.ordinary_roof_edits.begin(),roof.ordinary_roof_edits.end());
                    const auto opening_edits=roof_edits.empty() ? roof.roof_opening_edits : roof_edit_opening_intents(roof_edits);
                    for (const auto& edit:opening_edits)
                        for (const auto& upsert:edit.upserts)
                            if (fresh.contains(upsert.opening_id))
                                throw std::invalid_argument("Roof opening identity was already reserved by retained intent: "+upsert.opening_id);
                }
                if (!intent.structural_replacement.is_null()) {
                    const auto structural=decode_phase_structural_replacement_authoring(intent.structural_replacement);
                    require_unused(structural.identities);
                    require_unused(structural.hosted_instance_identities);
                }
                if (!intent.stair_replacement.is_null()) {
                    const auto stair=decode_phase_stair_replacement_authoring(intent.stair_replacement);
                    require_unused(stair.identities);
                    require_unused(stair.child_identities);
                    require_unused(stair.hosted_instance_identities);
                    require_unused(stair.overlay_identities);
                }
            }
        if (structural_hosted_alias_reservation)
            for (const auto& [qualified,alias]:embedded_assembly_presentation_ids(record.entities)) {
                (void)qualified;
                if (fresh.contains(alias))
                    throw std::invalid_argument("A proposed structural identity was reserved by a historical component presentation: "+alias);
            }
        for (const auto& [id,entity] : history.at(index).entities) {
            if (fresh.contains(id)) throw std::invalid_argument("Active design identity was already used in retained history: "+id);
            if ((hosted_slab_asset_reservation || roof_mixed_asset_reservation || structural_asset_reservation || wall_join_asset_reservation) &&
                entity.type=="assembly_model" && entity.properties.is_object()) {
                const auto model=entity.properties.find("model");
                if (model!=entity.properties.end() && model->is_object()) {
                    const auto instances=model->find("instances");
                    if (instances!=model->end() && instances->is_array())
                        for (const auto& instance:*instances)
                            if (instance.is_object() && instance.contains("id") && instance.at("id").is_string()) {
                                const auto alias=id+":instance:"+instance.at("id").get<std::string>();
                                if (fresh.contains(alias))
                                    throw std::invalid_argument("A proposed component identity was reserved by a retained presentation: "+alias);
                            }
                }
            }
            if (!nested_fresh.empty() && !wall_join_asset_reservation) {
                if (complete_envelope_reservation && nested_fresh.contains(entity.type))
                    throw std::invalid_argument("Proposed identity aliases a retained entity type: "+entity.type);
                // Openings, assembly layers and view overlays own identities
                // below the entity level. Retained metadata reserves names
                // too; Undo does not make any of them available again.
                std::size_t nodes=0,bytes=0;
                const auto reserve=[&](const auto& self,const nlohmann::json& value,unsigned depth)->void {
                    if (depth>64 || ++nodes>4*1024*1024)
                        throw std::invalid_argument("Retained identity nesting/node budget exceeded");
                    const auto text=[&](const std::string& name) {
                        if (name.size()>64*1024*1024-bytes)
                            throw std::invalid_argument("Retained identity string budget exceeded");
                        bytes+=name.size();
                        if (nested_fresh.contains(name))
                            throw std::invalid_argument("Proposed identity was already retained in history: "+name);
                    };
                    if (value.is_string()) text(value.template get_ref<const std::string&>());
                    else if (value.is_array()) for (const auto& child:value) self(self,child,depth+1);
                    else if (value.is_object()) for (const auto& [name,child]:value.items()) {
                        text(name);self(self,child,depth+1);
                    }
                };
                reserve(reserve,entity.properties,0);
                reserve(reserve,entity.extensions,0);
            }
            if (entity.type=="wall" && entity.properties.contains("layers"))
                for (const auto& layer:entity.properties.at("layers"))
                    if (fresh.contains(layer.at("id").get<std::string>()))
                        throw std::invalid_argument("Active design identity collides with a retained wall layer: "+id);
            if (entity.type=="measurement_linework") {
                const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
                if (decoded.supported()) for (const auto& edge : decoded.model->edges)
                    if (fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                        throw std::invalid_argument("Active design identity collides with retained linework: "+id);
            }
            if (!can_recognize_boundary_entity_type(entity.type) ||
                inspect_boundary_entity_version(entity).format!=BoundaryEntityFormat::identified_v1) continue;
            for (const auto& edge : decode_identified_boundary_entity(entity).segments)
                if (fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                    throw std::invalid_argument("Active design identity collides with retained boundary children: "+id);
        }
    }
}
#endif

// Current authoring cannot silently rewrite a baseline architectural object while an
// alternative is active. Historical ordinary records retain their original
// meaning: this guard is deliberately outside restore/replay validation.
static void validate_current_baseline_physical_preservation(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate) {
    for (const auto& [registry_id,registry]:source) {
        (void)registry_id;
        if (registry.type!="model_phases") continue;
        const auto model=ModelPhases::from_json(registry.properties.at("model"));
        if (!model.active_alternative()) continue;
        for (const auto& id:model.baseline_ids()) {
            const auto original=source.find(id);
            if (original==source.end() || (original->second.type!="roof" && original->second.type!="roof_join" &&
                original->second.type!="column" && original->second.type!="beam" &&
                original->second.type!="stair" && original->second.type!="railing")) continue;
            const auto after=candidate.find(id);
            if (after==candidate.end() || !exact_entity_payload(original->second,after->second))
                document_error(DocumentErrorCode::invalid_entity,
                    "Editing a baseline "+original->second.type+" in an alternative requires a proposed replacement: "+id);
        }
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
            a.vertex_id != b.vertex_id || a.secondary_segment_id != b.secondary_segment_id || a.segment_chain_ids != b.segment_chain_ids ||
            a.corner_leg != b.corner_leg)
            throw std::invalid_argument("Dimension placement cannot remap its stable analytical target");
        const auto old_owner = source.find(a.boundary_id);
        const auto new_owner = candidate.find(b.boundary_id);
        if (old_owner == source.end() || new_owner == candidate.end() || old_owner->second.type != new_owner->second.type)
            throw std::invalid_argument("Dimension placement analytical owner must survive with the same type");
        if (a.kind==BoundaryDimensionKind::corner_window_leg_length) {
            (void)a.resolve(source); (void)b.resolve(candidate);
        } else { (void)a.resolve(old_owner->second); (void)b.resolve(new_owner->second); }
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

static void retain_joint_callout_placement(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent) {
    const auto offsets = resolve_joint_translation_offsets(source, intent);
    const bool rigid_transform = intent.per_owner_rigid_completion || !intent.owner_transformations.empty();
    const bool per_owner = rigid_transform || intent.per_owner_translation_completion || !intent.owner_translations.empty() ||
        !intent.dimension_translations.empty();
    std::set<std::string, std::less<>> rigid(intent.rigid_boundary_ids.begin(), intent.rigid_boundary_ids.end());
    rigid.insert(intent.rigid_stroke_ids.begin(), intent.rigid_stroke_ids.end());
    if (per_owner)
        rigid.insert(intent.partial_wall_ids.begin(), intent.partial_wall_ids.end());
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
        const bool rigid_owner = rigid.contains(decoded.dimension->boundary_id);
        if (!rigid_owner && !((per_owner || intent.physical_room_dimension_completion) &&
            offsets.dimension_offsets.contains(id))) continue;
        const auto found = candidate.find(id);
        if (found == candidate.end()) throw std::invalid_argument("Joint translation retired a rigid-owner callout");
        if (intent.physical_room_dimension_completion && is_physical_wall_room(source.at(decoded.dimension->boundary_id)))
            (void)resolve_current_boundary_dimension(*decoded.dimension, candidate);
        auto placed = *decoded.dimension;
        if (rigid_owner && rigid_transform)
            placed.text_position = transform_point(placed.text_position, offsets.owner_transforms.at(placed.boundary_id));
        else {
            const auto offset = rigid_owner ? offsets.owner_offsets.at(placed.boundary_id) : offsets.dimension_offsets.at(id);
            placed.text_position = {placed.text_position.x + offset.x, placed.text_position.y + offset.y};
        }
        if (!std::isfinite(placed.text_position.x) || !std::isfinite(placed.text_position.y))
            throw std::invalid_argument("Joint translation callout position overflows");
        if (!rigid_owner) {
            placed.placement = BoundaryDimensionPlacement::manual;
            placed.automatic_placement_version.reset();
        }
        // A whole-owner move keeps automatic/manual placement provenance. The
        // ordinary placement lane's manual conversion applies to independent
        // callout drags, not this source-reconstructed rigid movement.
        found->second = encode_boundary_dimension_entity(placed, &entity);
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

static void complete_rigid_geometry_dimensions(const std::map<std::string,Entity,std::less<>>& source,
    std::map<std::string,Entity,std::less<>>& entities,
    const std::map<std::string,PlanarTransform,std::less<>>& transforms,
    bool wall_owners = false) {
    if(transforms.empty())return;
    for(const auto& [id,original]:source) {
        if(!can_recognize_boundary_dimension_entity_type(original.type))continue;
        const auto decoded=decode_boundary_dimension_entity(original);
        if(!decoded.supported()) {
            const auto target=original.properties.find("target");
            if(target!=original.properties.end() && target->is_object() && target->contains("entity_id") &&
               target->at("entity_id").is_string() && transforms.contains(target->at("entity_id").get<std::string>()))
                throw std::invalid_argument(wall_owners ? "Unsupported attached dimension cannot follow a rigid wall transform" :
                    "Unsupported attached dimension cannot follow a measured transform");
            continue;
        }
        const auto transform=transforms.find(decoded.dimension->boundary_id);
        if(transform==transforms.end())continue;
        const auto candidate=entities.find(id);
        if(candidate==entities.end() || candidate->second!=original)
            throw std::invalid_argument(wall_owners ? "Rigid wall transform overlaps an edit of its attached dimension" :
                "Measured transform overlaps an edit of its attached dimension");
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
            const auto& kind=record.at("target_kind");
            if((kind!="area" && kind!="area_name" && kind!="area_calculation") ||
               !record.contains("plan_label_offset_m"))continue;
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
    auto expected = replayed_wall_split_entities(known, intent, true);
    for (auto& [id, entity] : opaque) {
        if (!expected.emplace(id, std::move(entity)).second)
            throw std::invalid_argument("Retained wall split collides with an opaque relationship entity");
    }
    return expected;
}

static std::map<std::string, Entity, std::less<>> replay_retained_wall_merge(
    const std::map<std::string, Entity, std::less<>>& source, const WallMergeIntent& intent) {
    // Opaque future relationships cannot provide merge authority. Reconstruct
    // understood source independently and then restore their exact identities
    // and bytes before comparing the complete retained event.
    auto known = source;
    std::map<std::string, Entity, std::less<>> opaque;
    for (const auto& [id, entity] : source) {
        if (entity.type == "room_relationships" &&
            room_relationship_model_version(entity.properties.at("model")) > 2) {
            opaque.emplace(id, entity);
            known.erase(id);
        }
    }
    auto expected = replayed_wall_merge_entities(known, intent);
    for (auto& [id, entity] : opaque) {
        if (!expected.emplace(id, std::move(entity)).second)
            throw std::invalid_argument("Retained wall merge collides with an opaque relationship entity");
    }
    return expected;
}

void validate_completed_constraint_change(const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false) {
    if (has_independent_drawing_removal(command)) {
        // Both exact stages and their final state are admitted by complete
        // reconstruction; partial wall authority cannot validate their union.
        try { validate_independent_drawing_removal_mode(command); (void)command_to_json(Command{command}); }
        catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation,error.what()); }
        return;
    }
    if (has_phase_constraint_authoring(command) && !has_selection_completion(command) && !has_disto_measurement_completion(command)) {
        try {
            validate_phase_constraint_authoring_mode(command);
            (void)command_to_json(Command{command});
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            if (replay_phase_constraint_authoring(before,command.phase_constraint_authoring_intent)!=after)
                throw std::invalid_argument("Active design changes differ from semantic source reconstruction");
#else
            throw std::invalid_argument("Active design changes require the production constraint engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation,error.what()); }
        return;
    }
    if (has_phase_room_review_completion(command)) {
        try { validate_phase_room_review_mode(command); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation,error.what()); }
        // Complete replay preserves every original constraint and admits the
        // final state; the ordinary wall edit validator has no partial lane here.
        return;
    }
    // Both original-source lanes and their union are validated during complete
    // reconstruction; applying a child's partial authority to the union is unsafe.
    if (has_selection_completion(command)) return;
    try { validate_room_aware_wall_split_mode(command); validate_wall_merge_mode(command); validate_curve_construction_completion(command); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation, error.what()); }
    if (has_disto_measurement_completion(command)) {
        if (!command.disto_measurement_completion || !command.disto_measurement)
            document_error(DocumentErrorCode::constraint_violation, "DISTO completion requires its observation");
        auto geometry = after;
        const auto& source_id = command.disto_measurement->owner_id;
        const auto id=disto_completed_owner_id(without_disto_measurement(command),*command.disto_measurement);
        if (!before.contains(source_id) || !geometry.contains(id))
            document_error(DocumentErrorCode::constraint_violation, "DISTO completion owner is missing");
        restore_disto_measurements(before.at(source_id), geometry.at(id));
        auto expected = geometry;
        try { attach_disto_measurement(before, expected, *command.disto_measurement,id); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation, error.what()); }
        if (expected != after)
            document_error(DocumentErrorCode::constraint_violation, "DISTO completion differs from its observation");
        validate_completed_constraint_change(before, geometry, without_disto_measurement(command), retained_replay);
        return;
    }
    // Mixed transactions validate both original-source replays and the final
    // merged state in completed_boundary_constraint_entities. The legacy
    // validator must never apply partial authority to the rigid lane.
    if (has_rigid_group_completion(command) || has_joint_translation_completion(command) || has_room_review_completion(command)) return;
    try { validate_exterior_resize_related_edits(command); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::constraint_violation,error.what()); }
    if(command.wall_merge) {
        const auto expected = retained_replay ? replay_retained_wall_merge(before, *command.wall_merge) :
            replayed_wall_merge_entities(before, *command.wall_merge);
        if(expected!=after)document_error(DocumentErrorCode::constraint_violation,"Wall merge differs from complete source reconstruction");
        const auto normalized=wall_merge_validation_source(before,expected,*command.wall_merge);
        validate_constraint_change(normalized,after,true,true,false);
        return;
    }
    if(command.wall_split) {
        const auto expected = retained_replay ? replay_retained_wall_split(before, *command.wall_split) :
            replayed_wall_split_entities(before, *command.wall_split);
        if(expected!=after)document_error(DocumentErrorCode::constraint_violation,"Wall split differs from complete source reconstruction");
        const auto normalized=wall_split_validation_source(before,expected,*command.wall_split);
        validate_constraint_change(normalized,after,true,true,false);
        return;
    }
    std::set<std::string,std::less<>> rigid_ids;
    std::set<std::string,std::less<>> curve_construction_ids;
    for (const auto& edit : command.wall_edits) if (edit.version == 6) {
        const auto found = before.find(edit.wall_id);
        if (found == before.end() || !after.contains(edit.wall_id) ||
            !exact_entity_payload(replay_constraint_wall_edit(found->second,edit),after.at(edit.wall_id)))
            document_error(DocumentErrorCode::constraint_violation,"Curve construction differs from independently reconstructed source");
        curve_construction_ids.insert(edit.wall_id);
    }
    for(const auto& edit:command.wall_edits)if(edit.version==4 || edit.version==5) {
        const auto found=before.find(edit.wall_id);
        if(found==before.end() || !after.contains(edit.wall_id) ||
            !exact_entity_payload(replay_constraint_wall_edit(found->second,edit),after.at(edit.wall_id)))
            document_error(DocumentErrorCode::constraint_violation,"Rigid wall proof differs from independently reconstructed source");
        rigid_ids.insert(edit.wall_id);
    }
    if (!has_exterior_source_completion(command)) {
        validate_constraint_change(before, after, true, true, false, rigid_ids,curve_construction_ids);
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
        validate_constraint_wall_geometry_transition(typed_before, after, true,false,curve_construction_ids);
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
        throw std::invalid_argument("Automatic exterior source updates require typed retained-topology redraws");
}

// This visitor only vetoes reuse of identities in already validated, supported
// provenance. It never treats a receipt or a same-named vendor field as edit
// authority, and never traverses opaque extensions or local alternative IDs.
class WallSplitRetainedIdentityCheck {
public:
    WallSplitRetainedIdentityCheck(const std::set<std::string>& fresh, const Entity& owner)
        : fresh_(fresh), owner_(owner) {}

    void boundary_provenance() {
        if (const auto authoring = owner_.properties.find("boundary_authoring"); authoring != owner_.properties.end())
            (void)construction(*authoring);
        const auto derivation = owner_.extensions.find("boundary_geometry_derivation");
        if (derivation == owner_.extensions.end()) return;
        bounded(*derivation);
        if (!derivation->is_object() || derivation->size() != 3 || !derivation->contains("version") ||
            !derivation->at("version").is_number_integer() || !derivation->contains("operations") ||
            !derivation->at("operations").is_array()) return;
        if (derivation->at("version") == 1) {
            if (!derivation->contains("source_boundary_authoring") ||
                !construction(derivation->at("source_boundary_authoring"))) return;
        } else if (derivation->at("version") == 2) {
            if (!derivation->contains("source_boundary")) return;
            const auto& origin = derivation->at("source_boundary");
            if (!origin.is_object() || origin.size() != 2 || !origin.contains("boundary_model_version") ||
                !origin.at("boundary_model_version").is_number_integer() || origin.at("boundary_model_version") != 1 ||
                !origin.contains("segments")) return;
            segments(origin.at("segments"));
        } else return;
        for (const auto& operation : derivation->at("operations")) {
            rows(1);
            if (!operation.is_object() || operation.size() != 2 || !operation.contains("kind") ||
                !operation.at("kind").is_string() || !operation.contains("value")) return;
            const auto& kind = operation.at("kind").get_ref<const std::string&>();
            const auto& value = operation.at("value");
            if (kind == "geometry_edit") edit(decode_boundary_geometry_edit(value));
            else if (kind == "vertex_batch") {
                if (!value.is_array()) return;
                for (const auto& member : value) { rows(1); edit(decode_boundary_geometry_edit(member)); }
            } else if (kind == "transform") identity(decode_boundary_transform(value).boundary_id);
            else if (kind == "wall_merge") {
                if (!fields(value, {"version", "vertex_id", "segments", "wall_source_ids", "removed_wall_id"})) return;
                identity(value.at("vertex_id"));
                identity(value.at("removed_wall_id"));
                identities(value.at("wall_source_ids"));
                segments(value.at("segments"));
            } else if (kind == "physical_room_wall_merge") {
                const bool current_phase=value.is_object() && value.contains("version") &&
                    value.at("version").is_number_integer() && value.at("version")==2;
                if (current_phase) {
                    if (!fields(value, {"version", "first_wall_id", "second_wall_id", "source_descriptor", "current_source_descriptor", "descriptor", "seam_vertex_ids", "segments"})) return;
                } else if (!fields(value, {"version", "first_wall_id", "second_wall_id", "source_descriptor", "descriptor", "seam_vertex_ids", "segments"})) return;
                identity(value.at("first_wall_id"));
                identity(value.at("second_wall_id"));
                identities(value.at("seam_vertex_ids"));
                room_descriptor(value.at("source_descriptor"));
                if (current_phase) room_descriptor(value.at("current_source_descriptor"));
                room_descriptor(value.at("descriptor"));
                segments(value.at("segments"));
            } else if (kind == "physical_room_wall_split") {
                const bool current_phase=value.is_object() && value.contains("version") &&
                    value.at("version").is_number_integer() && value.at("version")==2;
                if (current_phase) {
                    if (!fields(value, {"version", "wall_id", "second_wall_id", "fraction", "source_descriptor", "current_source_descriptor", "descriptor", "insertions", "segments"})) return;
                } else if (!fields(value, {"version", "wall_id", "second_wall_id", "fraction", "source_descriptor", "descriptor", "insertions", "segments"})) return;
                identity(value.at("wall_id"));
                identity(value.at("second_wall_id"));
                room_descriptor(value.at("source_descriptor"));
                if (current_phase) room_descriptor(value.at("current_source_descriptor"));
                room_descriptor(value.at("descriptor"));
                const auto& insertions = value.at("insertions");
                rows(insertions.size());
                for (const auto& insertion : insertions) {
                    identity(insertion.at("segment_id"));
                    identity(insertion.at("new_vertex_id"));
                    identity(insertion.at("new_segment_id"));
                }
                segments(value.at("segments"));
            } else return;
        }
    }

private:
    const std::set<std::string>& fresh_;
    const Entity& owner_;
    std::size_t rows_{};

    void rows(std::size_t count) {
        // Count semantic records, not coordinates, keys or other scalar nodes.
        // A legitimate 10,000-edge origin must fit this traversal budget.
        if (count > kMaximumJsonValues - rows_)
            throw std::invalid_argument("Wall split retained identity proof exceeds its work budget");
        rows_ += count;
    }
    static void bounded(const nlohmann::json& value) {
        // Match the retained physical-room proof envelope limit. The preceding
        // state's ordinary entity JSON limits have also already been checked.
        if (value.dump().size() > 16ULL * 1024ULL * 1024ULL)
            throw std::invalid_argument("Wall split retained identity proof exceeds 16 MiB");
    }
    void identity(const std::string& id) const {
        if (!id.empty() && fresh_.contains(id))
            throw std::invalid_argument("Wall split identity was already used in retained boundary provenance: " + owner_.id);
    }
    void identity(const nlohmann::json& value) const {
        if (!value.is_null()) identity(value.get_ref<const std::string&>());
    }
    void identities(const nlohmann::json& values) {
        rows(values.size());
        for (const auto& value : values) identity(value);
    }
    static bool keys(const nlohmann::json& value, std::initializer_list<const char*> keys) {
        if (!value.is_object() || value.size() != keys.size()) return false;
        return std::all_of(keys.begin(), keys.end(), [&](const auto* key) { return value.contains(key); });
    }
    static bool fields(const nlohmann::json& value, std::initializer_list<const char*> expected) {
        return keys(value, expected) && value.contains("version") &&
            value.at("version").is_number_integer() && value.at("version") == 1;
    }
    void segments(const nlohmann::json& values) {
        rows(values.size());
        for (const auto& edge : values) {
            identity(edge.at("segment_id"));
            identity(edge.at("start_vertex_id"));
            identity(edge.at("end_vertex_id"));
        }
    }
    bool construction(const nlohmann::json& value) {
        bounded(value);
        // Bound supported edge inventories before the strict decoder's replay.
        // Unknown replay envelopes retain their existing opaque decode policy.
        const auto version = inspect_boundary_receipt_envelope(value);
        const bool understood = version.format != BoundaryReceiptEnvelopeFormat::unsupported_version &&
            value.contains("replay_version") && value.at("replay_version").is_number_integer() &&
            value.at("replay_version") == boundary_receipt_replay_version;
        if (understood && value.contains("segments") && value.at("segments").is_array())
            rows(value.at("segments").size());
        const auto decoded = decode_boundary_receipt_envelope(value);
        if (!decoded.supported()) return false;
        identity(decoded.record->boundary_id);
        for (const auto& edge : decoded.record->edges) {
            identity(edge.segment_id);
            identity(edge.start_vertex_id);
            identity(edge.end_vertex_id);
        }
        return true;
    }
    void context(const nlohmann::json& value) {
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "level_id"})
            if (value.contains(key)) identity(value.at(key));
    }
    void room_lineage(const nlohmann::json& value) {
        if (!fields(value, {"version", "basis", "context", "physical_sources", "semantic_phases", "outer", "holes", "component_index"}) ||
            value.at("basis") != "physical_wall_clear") return;
        const auto& sources = value.at("physical_sources");
        const auto& phases = value.at("semantic_phases");
        if (!keys(value.at("context"), {"property_id", "building_id", "floor_id", "layer_id", "level_id"}) ||
            !sources.is_array() || !phases.is_array()) return;
        rows(sources.size() + phases.size());
        // Only the understood physical inventory and phase registry define
        // owned identities. Similar fields in other lineage shapes stay opaque.
        for (const auto& source : sources)
            if (!keys(source, {"owner_id", "segment_id", "baseline", "thickness_m", "source_elevation_m", "effective_elevation_m", "source_context", "vertical_placement"}) ||
                source.at("segment_id") != "baseline" || !source.at("owner_id").is_string() ||
                !keys(source.at("source_context"), {"property_id", "building_id", "floor_id", "layer_id"})) return;
        for (const auto& phase : phases) {
            if (!keys(phase, {"id", "active_alternative", "owners"}) || !phase.at("id").is_string() ||
                !phase.at("owners").is_array()) return;
            rows(phase.at("owners").size());
            for (const auto& owner : phase.at("owners"))
                if (!keys(owner, {"owner_id", "active_state"}) || !owner.at("owner_id").is_string()) return;
        }
        context(value.at("context"));
        for (const auto& source : sources) {
            identity(source.at("owner_id"));
            context(source.at("source_context"));
            // These sources are physical wall axes: their segment_id is the
            // local feature token "baseline", not a document child identity.
        }
        for (const auto& phase : phases) {
            identity(phase.at("id"));
            const auto& owners = phase.at("owners");
            for (const auto& owner : owners) identity(owner.at("owner_id"));
        }
        // Directed face uses refer only to the captured wall inventory above;
        // their edge indices and "baseline" tokens carry no new identities.
    }
    void room_descriptor(const nlohmann::json& value) {
        if (!fields(value, {"version", "selected_wall_id", "source_lineage", "holes"})) return;
        identity(value.at("selected_wall_id"));
        room_lineage(value.at("source_lineage"));
    }
    void edit(const BoundaryGeometryEdit& value) {
        identity(value.boundary_id);
        identity(value.target_id);
        identity(value.new_vertex_id);
        identity(value.new_segment_id);
        identity(value.new_dimension_id);
        if (!value.replacement_segments.is_null()) segments(value.replacement_segments);
        if (!value.replacement_authoring.is_null()) (void)construction(value.replacement_authoring);
        rows(value.replacement_dimension_ids.size() + value.replacement_removed_reference_ids.size() + value.replacement_wall_source_ids.size());
        for (const auto& id : value.replacement_dimension_ids) identity(id);
        for (const auto& id : value.replacement_removed_reference_ids) identity(id);
        for (const auto& id : value.replacement_wall_source_ids) identity(id);
        for (const auto* group : {"segments", "vertices"})
            if (value.replacement_child_mapping.contains(group)) {
                const auto& mapping = value.replacement_child_mapping.at(group);
                rows(mapping.size());
                for (const auto& [old_id, new_id] : mapping.items()) { identity(old_id); identity(new_id); }
            }
        if (value.replacement_linework_sources)
            for (const auto& edge : *value.replacement_linework_sources) {
                rows(edge.size());
                for (const auto& use : edge) { identity(use.at("owner_id")); identity(use.at("segment_id")); }
            }
        if (value.physical_wall_room_repair) {
            identity(value.physical_wall_room_repair->selected_wall_id);
            room_lineage(value.physical_wall_room_repair->reviewed_source_lineage);
        }
        if (value.wall_source_translation && value.wall_source_translation->contains("genesis")) {
            const auto& walls = value.wall_source_translation->at("genesis").at("walls");
            rows(walls.size());
            for (const auto& wall : walls) { identity(wall.at("id")); context(wall.at("context")); }
        }
    }
};

static void validate_wall_split_lifetime(const WallSplitIntent& intent,
    const std::vector<RevisionRecord>& history,std::size_t preceding_records) {
    std::set<std::string> fresh;
    const auto reserve = [&](const std::string& id) {
        if (!is_valid_identifier(id) || !fresh.insert(id).second)
            throw std::invalid_argument("Wall split fresh identities are invalid or overlap");
    };
    reserve(intent.second_wall_id);
    reserve(intent.seam_constraint_id);
    for(const auto& owner:intent.measured_owners) {
        reserve(owner.vertex_id);
        reserve(owner.segment_id);
        if (!owner.automatic_dimension_id.empty()) reserve(owner.automatic_dimension_id);
    }
    for (const auto& owner : intent.physical_room_owners) {
        for (const auto* ids : {&owner.new_segment_ids, &owner.new_vertex_ids})
            for (const auto& id : *ids) reserve(id);
    }
    for(std::size_t i=0;i<preceding_records;++i)for(const auto& [id,entity]:history[i].entities) {
        if(fresh.contains(id))throw std::invalid_argument("Wall split identity was already used in retained history: "+id);
        // Loose strokes share the analytical identity namespaces. Preserve
        // historical v1 replay while protecting all room-aware child IDs.
        if (intent.physical_room_completion && entity.type == "measurement_linework") {
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            if (decoded.supported())
                for (const auto& edge : decoded.model->edges)
                    if (fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                        throw std::invalid_argument("Wall split child identity was already used in retained linework history: " + id);
        }
        if (intent.physical_room_completion && entity.type == "wall" &&
            entity.extensions.contains("wall_merge_archive") &&
            entity.extensions.at("wall_merge_archive").at("version") == 1) {
            validate_wall_merge_archive(entity);
            std::vector<const nlohmann::json*> pending{&entity.extensions.at("wall_merge_archive")};
            while (!pending.empty()) {
                const auto* retained = pending.back();
                pending.pop_back();
                for (const auto& original : retained->at("sources")) {
                    if (fresh.contains(original.at("id").get<std::string>()))
                        throw std::invalid_argument("Wall split identity was already used in retained merge archive: " + id);
                    const auto& extensions = original.at("extensions");
                    if (extensions.contains("wall_merge_archive"))
                        pending.push_back(&extensions.at("wall_merge_archive"));
                }
            }
        }
        if(!can_recognize_boundary_entity_type(entity.type) ||
            inspect_boundary_entity_version(entity).format!=BoundaryEntityFormat::identified_v1)continue;
        if (intent.physical_room_completion)
            WallSplitRetainedIdentityCheck(fresh, entity).boundary_provenance();
        for(const auto& edge:decode_identified_boundary_entity(entity).segments)
            if(fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                throw std::invalid_argument("Wall split child identity was already used in retained history: "+id);
    }
}

#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
// A staged review may use the actual inventory immediately before its wall
// geometry stage. The receipt names admitted retained history; it supplies no
// projected entities and cannot borrow an unrelated earlier measurement.
static const std::map<std::string,Entity,std::less<>>* room_dimension_original_source(
    const std::vector<RevisionRecord>& records,std::size_t preceding_records,
    const std::map<std::string,Entity,std::less<>>& source,
    const ApplyBoundaryConstraintChanges& command) {
    if (!has_room_review_completion(command)) return nullptr;
    const std::map<std::string,Entity,std::less<>>* result=nullptr;
    std::optional<PhysicalWallRoomDimensionSource> receipt;
    for (const auto& encoded:room_review_intents(command)) {
        const auto intent=decode_physical_wall_room_review_intent(encoded);
        if (intent.selected_dimension_placements.empty()) continue;
        if (!intent.selected_dimension_source)
            throw std::invalid_argument("Selected room callouts require their retained original source receipt");
        const auto& selected=*intent.selected_dimension_source;
        if (receipt && (receipt->original_revision!=selected.original_revision ||
            receipt->original_entities_digest!=selected.original_entities_digest))
            throw std::invalid_argument("Room callout placements disagree on their actual original source");
        receipt=selected;
        if (selected.original_revision==command.expected_revision) {
            if (selected.original_entities_digest!=entity_map_digest(source))
                throw std::invalid_argument("Room callout original inventory changed");
            result=&source;
            continue;
        }
        if (has_room_review_geometry_completion(command) || selected.original_revision>=command.expected_revision ||
            command.expected_revision>=preceding_records || preceding_records>records.size())
            throw std::invalid_argument("Room callout receipt does not name the original wall-edit source");
        const auto origin_index=static_cast<std::size_t>(selected.original_revision);
        const auto last_index=static_cast<std::size_t>(command.expected_revision);
        const auto& origin=records.at(origin_index);
        if (origin.revision!=selected.original_revision ||
            entity_map_digest(origin.entities)!=selected.original_entities_digest ||
            records.at(last_index).revision!=command.expected_revision ||
            records.at(last_index).entities.size()!=source.size())
            throw std::invalid_argument("Room callout retained source receipt changed");
        for (const auto& [id,entity]:source) {
            const auto found=records.at(last_index).entities.find(id);
            if (found==records.at(last_index).entities.end() || !exact_entity_payload(entity,found->second))
                throw std::invalid_argument("Room callout stage differs from actual retained history");
        }
        for (std::size_t index=origin_index+1;index<=last_index;++index) {
            const auto& record=records.at(index);
            if (record.revision!=index || record.parent_revision!=Revision{index-1} || record.source_revision ||
                record.name || record.assets!=origin.assets || record.boundary_translation || record.boundary_transform ||
                record.boundary_geometry_edit || record.boundary_translations || record.boundary_transforms || record.phase_entity_import)
                throw std::invalid_argument("Room callout stage contains navigation, naming or unrelated authoring");
            if (index==origin_index+1) {
                if (record.boundary_constraint_changes) {
                    const Command geometry{*record.boundary_constraint_changes};
                    if (!is_physical_wall_room_geometry_review_command(geometry) ||
                        record.boundary_constraint_changes->expected_revision!=origin.revision)
                        throw std::invalid_argument("Room callout anchor must immediately precede an admitted wall geometry stage");
                } else {
                    // Raw profile stages have no retained typed command. Only
                    // one exact existing-wall upsert can reconstruct that lane.
                    std::vector<EntityChange> changes;
                    if (record.entities.size()!=origin.entities.size())
                        throw std::invalid_argument("Room callout raw geometry stage changes object lifetimes");
                    for (const auto& [id,entity]:origin.entities) {
                        const auto found=record.entities.find(id);
                        if (found==record.entities.end())
                            throw std::invalid_argument("Room callout raw geometry stage lost an original owner");
                        if (!exact_entity_payload(entity,found->second)) {
                            if (entity.type!="wall" || found->second.type!="wall" || !changes.empty())
                                throw std::invalid_argument("Room callout raw geometry stage is not one wall profile edit");
                            changes.push_back(EntityChange::upsert(found->second));
                        }
                    }
                    const Command geometry{ApplyEntityChanges{origin.revision,std::move(changes),{},record.action}};
                    if (!is_physical_wall_room_profile_review_command(geometry))
                        throw std::invalid_argument("Room callout raw geometry stage has no profile authority");
                    validate_physical_wall_room_profile_review_source(origin.entities,record.entities,geometry);
                }
            } else {
                if (!record.boundary_constraint_changes)
                    throw std::invalid_argument("Room callout stages may contain only plain room reviews after geometry");
                const auto& review=*record.boundary_constraint_changes;
                validate_room_review_mode(review,true);
                const auto proof=command_to_json(Command{review});
                if ((proof.at("version")!=18 && proof.at("version")!=29) ||
                    has_room_review_geometry_completion(review) || has_room_review_batch_completion(review))
                    throw std::invalid_argument("Room callout stages cannot borrow a composed or unrelated review");
            }
            for (const auto& placement:intent.selected_dimension_placements) {
                const auto& dimension=origin.entities.at(placement.dimension_id);
                const auto decoded=decode_boundary_dimension_entity(dimension);
                if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
                for (const auto& id:{placement.dimension_id,decoded.dimension->boundary_id}) {
                    const auto original=origin.entities.find(id),current=record.entities.find(id);
                    if (original==origin.entities.end() || current==record.entities.end() ||
                        !exact_entity_payload(original->second,current->second))
                        throw std::invalid_argument("A prior room stage changed a selected callout or its original room");
                }
            }
        }
        result=&origin.entities;
    }
    return result;
}

static void validate_room_review_lifetime(const nlohmann::json& encoded,
    const std::vector<RevisionRecord>& history,std::size_t preceding_records) {
    const auto intent=decode_physical_wall_room_review_intent(encoded);
    std::set<std::string,std::less<>> fresh;
    const auto reserve=[&](const std::string& id) {
        if (!is_valid_identifier(id) || !fresh.insert(id).second)
            throw std::invalid_argument("Room review fresh identities are invalid or overlap");
    };
    for (const auto& decision:intent.fresh) {
        if (decision.disposition==PhysicalWallRoomFreshDisposition::unclassified) continue;
        if (decision.disposition==PhysicalWallRoomFreshDisposition::create) reserve(decision.room_id);
        for (const auto& id:decision.fresh_ids.segment_ids) reserve(id);
        for (const auto& id:decision.fresh_ids.vertex_ids) reserve(id);
    }
    for (const auto& decision:intent.retained)
        for (const auto& id:decision.replacement_dimension_ids) reserve(id);
    for (std::size_t index=0;index<preceding_records;++index) {
        for (const auto& [id,entity]:history[index].entities) {
            if (fresh.contains(id)) throw std::invalid_argument("Room review identity was already used in retained history: "+id);
            if (!can_recognize_boundary_entity_type(entity.type) ||
                inspect_boundary_entity_version(entity).format!=BoundaryEntityFormat::identified_v1) continue;
            for (const auto& edge:decode_identified_boundary_entity(entity).segments)
                if (fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                    throw std::invalid_argument("Room review child identity was already used in retained history: "+id);
        }
    }
}

static void validate_phase_room_review_lifetime(const nlohmann::json& encoded,
    const std::vector<RevisionRecord>& history,std::size_t preceding_records) {
    const auto intent=decode_physical_wall_phase_room_review_intent(encoded);
    std::set<std::string,std::less<>> fresh;
    const auto reserve=[&](const std::string& id) {
        if (!is_valid_identifier(id) || !fresh.insert(id).second)
            throw std::invalid_argument("Phase room review fresh identities are invalid or overlap");
    };
    if (!intent.source_registry_entity_digest) reserve(intent.registry_id);
    for (const auto& plane:intent.planes) {
        for (const auto& decision:plane.fresh) {
            if (decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::create_proposed &&
                decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) continue;
            if (decision.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed)
                reserve(decision.room_id);
            for (const auto& id:decision.fresh_ids.segment_ids) reserve(id);
            for (const auto& id:decision.fresh_ids.vertex_ids) reserve(id);
        }
        for (const auto& decision:plane.source_rooms)
            for (const auto& id:decision.replacement_dimension_ids) reserve(id);
    }
    for (std::size_t index=0;index<preceding_records;++index)
        for (const auto& [id,entity]:history[index].entities) {
            if (fresh.contains(id))
                throw std::invalid_argument("Phase room identity was already used in retained history: "+id);
            if (entity.type=="measurement_linework") {
                const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
                if (decoded.supported())
                    for (const auto& edge:decoded.model->edges)
                        if (fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                            throw std::invalid_argument("Phase room child identity was already used in retained linework: "+id);
            }
            if (!can_recognize_boundary_entity_type(entity.type) ||
                inspect_boundary_entity_version(entity).format!=BoundaryEntityFormat::identified_v1) continue;
            for (const auto& edge:decode_identified_boundary_entity(entity).segments)
                if (fresh.contains(edge.segment_id) || fresh.contains(edge.start_vertex_id) || fresh.contains(edge.end_vertex_id))
                    throw std::invalid_argument("Phase room child identity was already used in retained history: "+id);
        }
}
#endif

std::map<std::string, Entity, std::less<>> boundary_constraint_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false, bool prepared_rigid_geometry = false,
    const JointTranslationIntent* rigid_joint_intent = nullptr) {
    try { validate_room_aware_wall_split_mode(command); validate_wall_merge_mode(command); validate_exterior_resize_related_edits(command); validate_dimension_placement_intent(command, true); validate_wall_dimension_completion(command); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    if(command.wall_merge) {
        (void)command_to_json(Command{command});
        try{return retained_replay ? replay_retained_wall_merge(source, *command.wall_merge) :
            replayed_wall_merge_entities(source,*command.wall_merge);}
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    if(command.wall_split) {
        (void)command_to_json(Command{command});
        try{return retained_replay ? replay_retained_wall_split(source, *command.wall_split) :
            replayed_wall_split_entities(source,*command.wall_split);}
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    const bool source_completion = has_exterior_source_completion(command);
    const bool measured_completion=has_measured_source_completion(command);
    if (source_completion || measured_completion || has_dimension_placement_completion(command) || command.wall_dimension_completion)
        (void)command_to_json(Command{command});
    if (command.boundary_edits.empty() && command.wall_edits.empty() && !source_completion && !measured_completion && !prepared_rigid_geometry)
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
        if(edit.version==4 || edit.version==5)rigid_wall_ids.insert(edit.wall_id);
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
    try { complete_corner_window_geometry(source,result); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    for (const auto& edit : command.wall_edits) {
        try { validate_constraint_wall_host(edit.wall_id, result); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    try {
        if (command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc) validate_exterior_corner_edit_topology(source,result);
        else if (!prepared_rigid_geometry) validate_constraint_edit_topology(source,result,rigid_wall_ids);
    }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    if (command.curve_construction_completion) {
        try { validate_exterior_corner_physical_contacts(source,result); }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    if (source_completion) {
        try {
            if (command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc) validate_exterior_corner_physical_contacts(source, result);
            if (command.exterior_source_edits.empty())
                throw std::invalid_argument("Exterior source completion requires explicit redraws");
            std::map<std::string, Vec2, std::less<>> rigid_source_offsets;
            for (const auto& edit : command.exterior_source_edits) {
                if (!edit.wall_source_translation) continue;
                validate_exterior_source_redraw(edit);
                const auto& encoded_offset = edit.wall_source_translation->at("offset");
                const Vec2 offset{encoded_offset.at(0).get<double>(), encoded_offset.at(1).get<double>()};
                if (!rigid_source_offsets.emplace(edit.boundary_id, offset).second)
                    throw std::invalid_argument("Physical source translation owner is repeated");
            }
            std::map<std::string,PlanarTransform,std::less<>> rigid_source_transforms;
            if (rigid_joint_intent) {
                const auto resolved = resolve_joint_translation_offsets(source,*rigid_joint_intent);
                for (const auto& id : rigid_joint_intent->rigid_boundary_ids)
                    if (source.at(id).properties.contains("wall_measurement_source"))
                        rigid_source_transforms.emplace(id,resolved.owner_transforms.at(id));
                complete_joint_rigid_sources(source,result,*rigid_joint_intent,false);
            }
            const auto expected = exterior_wall_measurement_source_updates(source, result, !prepared_rigid_geometry,
                rigid_source_offsets, rigid_source_transforms);
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
            complete_rigid_geometry_dimensions(source,result,transforms);
            complete_measured_stroke_annotations(source,result,command);
        }
        catch(const std::exception& error){document_error(DocumentErrorCode::invalid_entity,error.what());}
    }
    if (command.wall_dimension_completion) {
        try {
            std::map<std::string,PlanarTransform,std::less<>> transforms;
            for (const auto& edit : command.wall_edits) transforms.emplace(edit.wall_id,*edit.rigid_transform);
            complete_rigid_geometry_dimensions(source,result,transforms,true);
        } catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
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
    if (constrained && (constrained->wall_merge || has_exterior_source_completion(*constrained))) {
        // Exclusive merge replay already completes its measured source owners.
        // It cannot acquire an ordinary exterior-completion payload afterwards.
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
    complete_rigid_geometry_dimensions(source,result,measured_transforms);
    validate_boundary_change(history, intermediate, result);
    try { validate_boundary_identity_transition(history, source, result); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    return result;
}

struct RigidExteriorAlignment { std::size_t offset{}; bool reversed{}; };
Boundary uniquely_aligned_rigid_exterior(const Boundary& expected, const Boundary& derived,
                                        const PlanarTransform& transform, RigidExteriorAlignment* correspondence = nullptr) {
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
                if (correspondence) *correspondence = {offset,reverse};
            }
        }
    }
    if (matches != 1)
        throw std::invalid_argument("Rigid exterior requires one unique machine-precision analytical correspondence");
    return aligned;
}

bool per_owner_rigid_transform(const TransformBoundaries& command) {
    return command.per_owner_transform_completion || !command.source_transformations.empty();
}

bool identity_rigid_transform(const PlanarTransform& transform) {
    return transform.rotation_radians == 0.0 && !transform.flip_horizontal && !transform.flip_vertical &&
        transform.offset.x == 0.0 && transform.offset.y == 0.0;
}

void validate_rigid_owner_transform(const RigidOwnerTransformation& intent) {
    validate_boundary_transform({intent.owner_id, intent.transform});
    const auto& t = intent.transform;
    // Bound persisted operators before trigonometry, integer axis-lock replay,
    // and source reconstruction. Historical transform dialects are unchanged.
    if (std::abs(t.pivot.x) > 1e12 || std::abs(t.pivot.y) > 1e12 ||
        std::abs(t.offset.x) > 1e12 || std::abs(t.offset.y) > 1e12 ||
        std::abs(t.rotation_radians) > 1e6)
        throw std::invalid_argument("Rigid owner transform exceeds the supported parameter range");
}

std::map<std::string, Entity, std::less<>> rigid_boundary_targets(
    const std::map<std::string, Entity, std::less<>>& source, const TransformBoundaries& command) {
    if (!per_owner_rigid_transform(command))
        return transformed_boundary_entities_batch(source, command.transformations);
    // Wall/stroke-only rigid groups have no boundary targets. Every boundary
    // in a mixed group otherwise replays against the original source once.
    if (command.transformations.empty()) return source;
    return transformed_boundary_entities_per_owner_batch(source, command.transformations);
}

Entity replay_rigid_source_wall(const Entity& original, const PlanarTransform& transform) {
    const auto& value = original.properties.at("baseline");
    const Segment old{{value.at("start")[0].get<double>(), value.at("start")[1].get<double>()},
        {value.at("end")[0].get<double>(), value.at("end")[1].get<double>()}, value.value("sweep_radians", 0.0)};
    validate_wall_curve_input(original);
    validate_wall_length_input(original);
    if (identity_rigid_transform(transform)) return original;
    const auto baseline = transform_segment(old, transform);
    if (!std::isfinite(segment_length(baseline)) ||
        std::abs(segment_length(old) - segment_length(baseline)) > constraint_linear_tolerance_metres ||
        std::abs(old.sweep_radians) != std::abs(baseline.sweep_radians))
        throw std::invalid_argument("Rigid source wall must preserve its physical length and sweep magnitude");
    auto expected = original;
    transform_wall_curve_input(expected, transform);
    rebase_wall_length_receipt(expected, baseline);
    if (const auto plane = original.properties.find("top_plane"); plane != original.properties.end()) {
        const PlanarTransform basis{{}, transform.rotation_radians, transform.flip_horizontal, transform.flip_vertical, {}};
        const auto gradient = transform_point(parse_wall_top_plane(*plane), basis);
        expected.properties["top_plane"] = wall_top_plane_json(gradient);
        const double rise = gradient.x * (baseline.end.x - baseline.start.x) +
            gradient.y * (baseline.end.y - baseline.start.y);
        if (!std::isfinite(rise)) throw std::invalid_argument("Rigid wall top plane exceeds the supported range");
        expected.properties["slope_rise_m"] = rise;
        if (expected.properties.contains("slope_rise")) expected.properties["slope_rise"] = rise;
    }
    expected.properties["baseline"]["start"] = {baseline.start.x, baseline.start.y};
    expected.properties["baseline"]["end"] = {baseline.end.x, baseline.end.y};
    expected.properties["baseline"]["sweep_radians"] = baseline.sweep_radians;
    validate_wall_curve_input(expected);
    validate_wall_length_input(expected);
    return expected;
}

Entity replay_rigid_source_opening(const Entity& original, const PlanarTransform& transform) {
    auto expected=original;
    if (transform.flip_horizontal==transform.flip_vertical) return expected;
    if (expected.properties.contains("door_operation")) {
        auto operation=decode_door_operation(expected.properties.at("door_operation"));
        operation.swing_left=!operation.swing_left;
        expected.properties["door_operation"]=encode_door_operation(operation);
    }
    if (expected.properties.contains("opening_assembly")) {
        auto assembly=parse_opening_assembly(expected.properties.at("opening_assembly"));
        assembly.inset_m=-assembly.inset_m;
        if (assembly.window_layout==WindowLayoutKind::casement ||
            assembly.window_layout==WindowLayoutKind::sliding || assembly.window_layout==WindowLayoutKind::bay ||
            assembly.window_layout==WindowLayoutKind::bow || assembly.window_layout==WindowLayoutKind::awning ||
            assembly.window_layout==WindowLayoutKind::double_hung)
            assembly.window_open_left=!assembly.window_open_left;
        expected.properties["opening_assembly"]=opening_assembly_json(assembly);
    }
    return expected;
}

std::map<std::string, Entity, std::less<>> boundary_transform_entities(
    const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& source,
    const TransformBoundaries& command) {
    const bool per_owner = per_owner_rigid_transform(command);
    if (command.transformations.empty() && (!per_owner || command.source_transformations.empty()))
        document_error(DocumentErrorCode::invalid_entity,"Boundary transform group is empty");
    (void)command_to_json(command); // The retained proof must be persistable too.
    const PlanarTransform shared = command.transformations.empty() ? PlanarTransform{} : command.transformations.front().transform;
    std::set<std::string> owners, protected_ids, source_walls, moved_walls;
    std::map<std::string, PlanarTransform, std::less<>> owner_transforms, source_transforms;
    for (const auto& transformation : command.transformations) {
        validate_boundary_transform(transformation);
        if (per_owner) validate_rigid_owner_transform({transformation.boundary_id, transformation.transform});
        if (!per_owner && !(transformation.transform == shared))
            document_error(DocumentErrorCode::invalid_entity,"Boundary transform group requires one shared transform");
        if (!owners.insert(transformation.boundary_id).second)
            document_error(DocumentErrorCode::duplicate_change,"Boundary is transformed more than once");
        owner_transforms.emplace(transformation.boundary_id, transformation.transform);
    }
    for (const auto& intent : command.source_transformations) {
        validate_rigid_owner_transform(intent);
        const auto found = source.find(intent.owner_id);
        if (found == source.end() || (found->second.type != "wall" && found->second.type != "measurement_linework"))
            throw std::invalid_argument("Rigid source intent requires an existing wall or measured stroke");
        if (owners.contains(intent.owner_id) || !source_transforms.emplace(intent.owner_id, intent.transform).second)
            throw std::invalid_argument("Rigid source intents require unique disjoint owners");
        owner_transforms.emplace(intent.owner_id, intent.transform);
    }
    const auto compatible = [&](const std::string& first, const std::string& second) {
        return owner_transforms.contains(first) && owner_transforms.contains(second) &&
            owner_transforms.at(first) == owner_transforms.at(second);
    };
    const auto linework_owners = [](const Entity& entity) {
        std::set<std::string> ids;
        const auto collect = [&](const auto& self, const nlohmann::json& value) -> void {
            if (value.is_object()) {
                if (value.contains("owner_id")) {
                    if (!value.at("owner_id").is_string())
                        throw std::invalid_argument("Measured source owner identity is malformed");
                    ids.insert(value.at("owner_id").get<std::string>());
                }
                for (const auto& item : value.items()) self(self, item.value());
            } else if (value.is_array()) for (const auto& item : value) self(self, item);
        };
        for (const auto* key : {"measurement_linework_sources", "measurement_linework_group"})
            if (entity.extensions.contains(key)) collect(collect, entity.extensions.at(key));
        return ids;
    };
    // Use the semantic phase set, never drawing/view visibility, to establish
    // current source authority for selected measured faces and group cohorts.
    std::set<std::string, std::less<>> semantic_available;
    std::map<std::string, MeasurementLineworkSourceCheck, std::less<>> old_linework_checks;
    if (per_owner) {
        for (const auto& [id, entity] : source) { (void)entity; semantic_available.insert(id); }
        for (const auto& [id, entity] : source) {
            (void)id;
            if (entity.type != "model_phases") continue;
            const auto phases = ModelPhases::from_json(entity.properties.at("model"));
            const auto active = phases.active_state();
            for (const auto& member : phases.entity_ids())
                if (!active.contains(member) || active.at(member) == ModelPhase::demolished)
                    semantic_available.erase(member);
        }
        old_linework_checks = measurement_linework_source_checks(source, &semantic_available);
    }
    protected_ids = owners;
    const auto source_current = [&](const auto& entities, const Entity& owner) {
        return wall_measurement_source_current(entities, owner);
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
        if (per_owner && (owner.extensions.contains("measurement_linework_sources") ||
                          owner.extensions.contains("measurement_linework_group"))) {
            if (!semantic_available.contains(id) || !measurement_linework_source_current(old_linework_checks, owner))
                throw std::invalid_argument("Rigid measured area transform requires current source lineage");
            const auto cohort = linework_owners(owner);
            if (cohort.empty()) throw std::invalid_argument("Rigid measured area source cohort is empty");
            for (const auto& stroke : cohort)
                if (!source_transforms.contains(stroke) || source.at(stroke).type != "measurement_linework" ||
                    !compatible(id, stroke))
                    throw std::invalid_argument("Rigid measured area cohort requires complete compatible captured stroke operators");
        }
        if (const auto deductions = owner.properties.find("deduction_ids"); deductions != owner.properties.end()) {
            if (!deductions->is_array())
                throw std::invalid_argument("Measured deductions must be an array");
            std::set<std::string> unique;
            for (const auto& value : *deductions) {
                if (!value.is_string() || !owners.contains(value.get<std::string>()) || value == id ||
                    !unique.insert(value.get<std::string>()).second)
                    throw std::invalid_argument("Rigid transform must include every retained deduction exactly once");
                if (per_owner && !compatible(id, value.get<std::string>()))
                    throw std::invalid_argument("Rigid deductions require compatible captured owner operators");
            }
        }
        if (owner.properties.contains("wall_measurement_source")) {
            const auto ids = exterior_wall_measurement_source_ids(owner);
            // A transform cannot repair a source that was already stale.
            if (!source_current(source,owner))
                throw std::invalid_argument("Exterior transform requires current recorded source context");
            source_walls.insert(ids.begin(),ids.end());
            if (per_owner)
                for (const auto& wall : ids)
                    if (!compatible(id, wall))
                        throw std::invalid_argument("Rigid exterior sources require compatible captured owner operators");
        }
    }
    std::map<std::string, Entity, std::less<>> source_witnesses, opening_witnesses;
    if (per_owner) {
        // Complete source authority is established before any selected
        // boundary geometry is replayed. Intent cannot be supplied merely to
        // lend an operator to an unrelated supplemental payload.
        for (const auto& change : command.entity_changes) {
            if (change.kind != EntityChangeKind::upsert || !source_transforms.contains(change.entity.id)) continue;
            const auto& id = change.entity.id;
            const auto& original = source.at(id);
            Entity expected;
            if (original.type == "wall") expected = replay_rigid_source_wall(original, source_transforms.at(id));
            else {
                const auto decoded = decode_measurement_linework_model(original.properties.at("model"));
                if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
                expected = original;
                expected.properties["model"] = encode_measurement_linework_model(
                    transformed_measurement_linework(*decoded.model, source_transforms.at(id)));
            }
            if (expected != change.entity || !source_witnesses.emplace(id, std::move(expected)).second)
                throw std::invalid_argument("Rigid source witness differs from its exact captured-owner reconstruction");
        }
        if (source_witnesses.size() != source_transforms.size())
            throw std::invalid_argument("Rigid source intent is missing its exact supplemental witness");
        for (const auto& [id,entity]:source) {
            if (entity.type!="opening") continue;
            const auto& host=entity.properties.at("wall_id").get_ref<const std::string&>();
            if (!source_transforms.contains(host) || source.at(host).type!="wall") continue;
            opening_witnesses.emplace(id,replay_rigid_source_opening(entity,source_transforms.at(host)));
        }
    }
    auto intermediate = rigid_boundary_targets(source,command);
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
        if (per_owner && (change.entity.type == "wall" || change.entity.type == "measurement_linework") &&
            !source_transforms.contains(id))
            throw std::invalid_argument("Rigid supplemental geometry requires its own captured source intent");
        if (change.entity.type == "wall") {
            const auto read = [](const Entity& wall) {
                const auto& value = wall.properties.at("baseline");
                return Segment{{value.at("start")[0].get<double>(),value.at("start")[1].get<double>()},
                    {value.at("end")[0].get<double>(),value.at("end")[1].get<double>()},value.value("sweep_radians",0.0)};
            };
            const auto& transform = per_owner ? source_transforms.at(id) : shared;
            if (per_owner && source_witnesses.at(id) != change.entity)
                throw std::invalid_argument("Rigid source wall differs from its exact captured-owner reconstruction");
            const auto expected = per_owner && identity_rigid_transform(transform) ? read(previous->second) :
                transform_segment(read(previous->second), transform);
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
        } else if(change.entity.type=="measurement_linework" &&
            (per_owner || command.measured_stroke_transform_completion || has_measured_stroke_dimensions(source,id))) {
            const auto decoded=decode_measurement_linework_model(previous->second.properties.at("model"));
            if(!decoded.supported())throw std::invalid_argument(decoded.diagnostic);
            auto expected=previous->second;
            const auto& transform = per_owner ? source_transforms.at(id) : shared;
            expected.properties["model"]=encode_measurement_linework_model(transformed_measurement_linework(*decoded.model,transform));
            if(expected!=change.entity)throw std::invalid_argument("Measured stroke differs from the shared rigid transform");
            measured_transforms.emplace(id,transform);
        } else if (change.entity.type == "opening") {
            if (per_owner) {
                const auto& host = previous->second.properties.at("wall_id").get_ref<const std::string&>();
                if (!source_transforms.contains(host) || source.at(host).type != "wall")
                    throw std::invalid_argument("Rigid opening supplement requires its captured source wall");
                if (opening_witnesses.at(id) != change.entity)
                    throw std::invalid_argument("Rigid opening supplement differs from its exact host reflection consequence");
            }
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
    // A wall intent owns every hosted cut. Reconstruct omitted reflection
    // consequences as well as validating supplied witnesses, once from source.
    // Removing an opening payload cannot retain the wrong handedness or inset.
    for (const auto& [id,opening]:opening_witnesses) result.insert_or_assign(id,opening);
    for (const auto& [id, transform] : source_transforms) {
        (void)transform;
        if (!moved_walls.contains(id) && !measured_transforms.contains(id))
            throw std::invalid_argument("Rigid source intent is missing its exact supplemental witness");
    }
    const bool identity = per_owner ? std::all_of(owner_transforms.begin(), owner_transforms.end(),
        [](const auto& item) { return identity_rigid_transform(item.second); }) : identity_rigid_transform(shared);
    for (const auto& id : source_walls)
        if (!moved_walls.contains(id) && !identity)
            throw std::invalid_argument("Rigid exterior transform must include every source wall");
    std::set<std::string> transformed_ids = owners;
    if (!per_owner && command.wall_dimension_completion && moved_walls.empty())
        throw std::invalid_argument("Rigid wall callout completion requires a validated wall witness");
    if (!per_owner && command.measured_stroke_transform_completion && measured_transforms.empty())
        throw std::invalid_argument("Rigid measured completion requires a validated stroke witness");
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
        const auto& transform = per_owner ? owner_transforms.at(bindings.front().owner_id) : shared;
        if (per_owner && !std::all_of(bindings.begin(), bindings.end(), [&](const auto& value) {
                return owner_transforms.at(value.owner_id) == transform;
            }))
            throw std::invalid_argument("Rigid hard relations require compatible captured owner operators");
        auto expected = *decoded.constraint;
        if (expected.anchor && (!per_owner || expected.relation!=ConstraintRelationKind::fixed_anchor))
            expected.anchor = transform_point(*expected.anchor, transform);
        if (expected.relation == ConstraintRelationKind::horizontal || expected.relation == ConstraintRelationKind::vertical) {
            if (std::abs(std::remainder(transform.rotation_radians,std::numbers::pi/2)) > 1e-12)
                throw std::invalid_argument("Axis-locked relations require a quarter-turn rigid rotation");
            if (std::llround(transform.rotation_radians/(std::numbers::pi/2)) % 2 != 0)
                expected.relation = expected.relation == ConstraintRelationKind::horizontal ? ConstraintRelationKind::vertical : ConstraintRelationKind::horizontal;
        }
        if (result.at(id) != encode_constraint_entity(expected,&entity))
            throw std::invalid_argument("Rigid transform must preserve constraint endpoint identities, values and metadata");
    }
    // Reject ordinary receipt edits before any canonical source reconciliation.
    complete_rigid_geometry_dimensions(source,result,measured_transforms);
    std::map<std::string,PlanarTransform,std::less<>> wall_transforms;
    for (const auto& id : moved_walls) wall_transforms.emplace(id,per_owner ? source_transforms.at(id) : shared);
    // Only independently validated wall baselines lend placement
    // authority. Raw dimension supplements remain forbidden above.
    if (per_owner || command.wall_dimension_completion)
        complete_rigid_geometry_dimensions(source,result,wall_transforms,true);
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
        const auto aligned = uniquely_aligned_rigid_exterior(boundary_geometry(boundary),exterior.boundary,owner_transforms.at(id));
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
    if (per_owner && !old_linework_checks.empty()) {
        const auto checks = measurement_linework_source_checks(result, &semantic_available);
        for (const auto& id : owners) {
            const auto found = checks.find(id);
            if (found == checks.end()) continue;
            const auto& check = found->second;
            if (!check.proposed_boundary || !check.proposed_lineage.is_array())
                throw std::invalid_argument("Rigid measured source has no unique final face correspondence: " + check.diagnostic);
            auto boundary = decode_identified_boundary_entity(result.at(id));
            const auto aligned = uniquely_aligned_rigid_exterior(
                boundary_geometry(boundary), *check.proposed_boundary, owner_transforms.at(id));
            bool exact = true;
            for (std::size_t index = 0; index < aligned.size(); ++index) {
                const auto& a = boundary.segments[index].segment;
                const auto& b = aligned[index];
                exact = exact && a.start.x == b.start.x && a.start.y == b.start.y &&
                    a.end.x == b.end.x && a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
                boundary.segments[index].segment = b;
            }
            // Retain the rigid proof, then archive only the uniquely matched
            // machine-roundoff correction needed by exact source-face replay.
            if (!exact) {
                BoundaryGeometryEdit edit;
                edit.boundary_id = id; edit.target_id = id;
                edit.kind = BoundaryGeometryEditKind::redefine_boundary;
                edit.replacement_segments = encode_identified_boundary_entity(boundary).properties.at("segments");
                result = edited_boundary_entities(result, edit);
            }
            auto& owner = result.at(id);
            owner.extensions["measurement_linework_sources"] = check.proposed_lineage;
            if (owner.extensions.contains("measurement_linework_group")) {
                if (!check.proposed_group.is_object())
                    throw std::invalid_argument("Rigid measured group requires complete reconstructed member lineage");
                owner.extensions["measurement_linework_group"] = check.proposed_group;
            }
        }
        const auto verified = measurement_linework_source_checks(result, &semantic_available);
        for (const auto& [id, old] : old_linework_checks) {
            const auto referenced = linework_owners(source.at(id));
            const bool affected = owners.contains(id) || std::any_of(referenced.begin(), referenced.end(),
                [&](const auto& stroke) { return measured_transforms.contains(stroke); });
            if (!affected || !old.current) continue;
            const auto found = verified.find(id);
            if (found == verified.end() || !found->second.current)
                throw std::invalid_argument("Rigid transform would stale a measured source consumer: " + id);
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
    if (per_owner) {
        // Canonical source reconciliation may reflow an automatic callout.
        // Its v3 placement remains owned by the captured boundary operator.
        for (const auto& [id, entity] : intermediate) {
            if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported() || !owners.contains(decoded.dimension->boundary_id)) continue;
            const auto current = result.find(id);
            if (current == result.end() || !decode_boundary_dimension_entity(current->second).supported())
                throw std::invalid_argument("Rigid source reconciliation lost an attached analytical callout");
            if (decoded.dimension->kind==BoundaryDimensionKind::corner_window_leg_length)
                (void)decoded.dimension->resolve(result);
            else (void)decoded.dimension->resolve(result.at(decoded.dimension->boundary_id));
            current->second = entity;
        }
    }
    validate_boundary_identity_transition(history,source,result);
    return result;
}

std::map<std::string, Entity, std::less<>> completed_boundary_constraint_entities(
    const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Asset, std::less<>>& source_assets,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false,
    bool active_phase_constraints = false,
    const std::map<std::string,Entity,std::less<>>* original_dimension_source = nullptr) {
    const bool active_policy=active_phase_constraints || !phase_constraint_authoring_proofs(command).empty();
    if (has_independent_drawing_removal(command)) {
        try {
            validate_independent_drawing_removal_mode(command);
            (void)command_to_json(Command{command});
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            const auto intent=decode_drawing_selection_removal_intent(command.independent_drawing_removal_intent);
            // Source/selection and opaque-reference admission are analytical
            // and must precede the original wall/room geometry reconstruction.
            (void)replay_drawing_selection_removal(source,intent,active_policy);
            if (has_complete_wall_join_deletion_proof(command))
                for (const auto& [owner,ids]:complete_wall_join_deletion_destinations(command)) {
                    (void)owner;
                    for (const auto& id:ids)
                        if (id=="independent_drawing_removal_completion" || id=="independent_drawing_removal_intent" ||
                            id=="owner_ids" || id=="annotations" || id=="owner_id" || id=="child_id")
                            throw std::invalid_argument("A fresh removal destination borrows an independent drawing proof token");
                }
            const auto original=without_independent_drawing_removal(command);
            const auto stage=completed_boundary_constraint_entities(history,source,source_assets,
                original,retained_replay,active_policy,original_dimension_source);
            validate_completed_constraint_change(source,stage,original,retained_replay);
            auto result=replay_drawing_selection_removal_after_review(source,stage,intent,active_policy);
            if (has_phase_constraint_authoring(original)) {
                // Explicit drawing retirement may remove active annotation
                // rows, but cannot alter suspended constraints, inactive bodies
                // or the retained order/values of inactive annotation rows.
                validate_active_design_preserved_dependents(source,result,
                    false,true);
                validate_active_design_preserved_dependents(stage,result,false,true);
            }
            validate_boundary_identity_transition(history,source,result);
            (void)validate_state(result,source_assets,active_policy);
            return result;
#else
            throw std::invalid_argument("Independent drawing removal requires the production authoring engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    if (has_phase_constraint_authoring(command) && !has_selection_completion(command) && !has_disto_measurement_completion(command)) {
        try {
            validate_phase_constraint_authoring_mode(command);
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            auto result=replay_phase_constraint_authoring(source,command.phase_constraint_authoring_intent);
            validate_active_design_preserved_dependents(source,result,
                phase_constraint_authoring_preserves_registries(command),
                phase_constraint_authoring_retires_proposals(command));
            validate_phase_constraint_composed_originals(source,result,command);
            validate_boundary_identity_transition(history,source,result);
            (void)validate_state(result,source_assets,true);
            return result;
#else
            throw std::invalid_argument("Active design authoring requires the production constraint engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    if (has_phase_room_review_completion(command)) {
        try {
            validate_phase_room_review_mode(command);
            (void)command_to_json(Command{command});
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
            const auto replay=replay_physical_wall_phase_room_review(source,command.phase_room_review_intent,active_policy);
            validate_boundary_identity_transition(history,source,replay.entities);
            (void)validate_state(replay.entities,source_assets,active_policy);
            return replay.entities;
#else
            throw std::invalid_argument("Phase room review requires the production physical-wall engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    if (has_selection_completion(command)) {
        try {
            (void)command_to_json(Command{command});
            validate_selection_changes(source, command);
            const auto geometry = without_selection_completion(command);
            bool rigid_annotation_merge=geometry.joint_translation &&
                (geometry.joint_translation->per_owner_rigid_completion || !geometry.joint_translation->owner_transformations.empty());
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            if (has_phase_constraint_authoring(geometry)) {
                const auto intent=decode_phase_constraint_authoring_intent(geometry.phase_constraint_authoring_intent);
                rigid_annotation_merge=intent.intent.joint_translation &&
                    (intent.intent.joint_translation->per_owner_rigid_completion ||
                        !intent.intent.joint_translation->owner_transformations.empty());
            }
#endif
            auto result = completed_boundary_constraint_entities(history, source, source_assets, geometry, retained_replay,active_policy,original_dimension_source);
            const auto geometry_assets = boundary_constraint_assets(source_assets, geometry);
            (void)validate_state(result, geometry_assets,active_policy);
            validate_completed_constraint_change(source, result, geometry, retained_replay);
            validate_physical_room_source_transition(source, result, nullptr, &geometry,active_policy);
            const auto ordinary = ordinary_entity_changes(source, command.selection_entity_changes);
            (void)validate_state(ordinary, source_assets,active_policy);
            validate_constraint_change(source, ordinary);
            validate_boundary_change(history, source, ordinary);
            validate_physical_room_source_transition(source, ordinary);
            for (const auto& change : command.selection_entity_changes) {
                const auto& original = source.at(change.entity.id);
                const auto& admitted = ordinary.at(change.entity.id);
                if (exact_entity_payload(original, admitted)) continue;
                const auto current = result.find(change.entity.id);
                if (current == result.end()) throw std::invalid_argument("Selection completion lost a dependency identity");
                if (!exact_entity_payload(current->second, original) && !exact_entity_payload(current->second, admitted)) {
                    if (original.type != kAnnotationEntityType || !rigid_annotation_merge)
                        throw std::invalid_argument("Selection lanes require conflicting final dependency payloads: " + change.entity.id);
                    current->second = merge_selection_annotation_entities(original,current->second,admitted,
                        AnnotationMergeMode::source_callout_geometry);
                } else current->second = admitted;
            }
            (void)validate_state(result, geometry_assets,active_policy);
            if (!phase_constraint_authoring_proofs(geometry).empty()) {
                validate_active_design_preserved_dependents(source,result,phase_constraint_authoring_preserves_registries(geometry),
                    phase_constraint_authoring_retires_proposals(geometry));
                validate_phase_constraint_composed_originals(source,result,geometry);
            }
            validate_physical_room_source_transition(source, result, nullptr, &geometry,active_policy,true);
            return result;
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    try { validate_room_aware_wall_split_mode(command); validate_wall_merge_mode(command); validate_wall_dimension_completion(command);
        validate_curve_construction_completion(command); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    if (has_disto_measurement_completion(command)) {
        try {
            (void)command_to_json(Command{command});
            auto result = completed_boundary_constraint_entities(history, source, source_assets, without_disto_measurement(command), retained_replay,active_policy,original_dimension_source);
            attach_disto_measurement(source, result, *command.disto_measurement,
                disto_completed_owner_id(without_disto_measurement(command),*command.disto_measurement));
            if (!phase_constraint_authoring_proofs(command).empty()) {
                validate_active_design_preserved_dependents(source,result,phase_constraint_authoring_preserves_registries(command),
                    phase_constraint_authoring_retires_proposals(command));
                validate_phase_constraint_composed_originals(source,result,command);
            }
            return result;
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    if (has_room_review_completion(command)) {
        try {
            validate_room_review_mode(command, true);
            (void)command_to_json(Command{command});
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
            // Selected room callouts borrow measurement authority only from
            // this actual original inventory, before a wall edit can stale it.
            for (const auto& encoded : room_review_intents(command)) {
                const auto intent=decode_physical_wall_room_review_intent(encoded);
                if (!intent.selected_dimension_placements.empty())
                    validate_physical_wall_room_dimension_placements(original_dimension_source ? *original_dimension_source : source,
                        intent.selected_dimension_placements);
            }
            auto reviewed_source=source;
            if (has_room_review_geometry_completion(command)) {
                const auto geometry=room_review_geometry_command(command);
                if (const auto* constrained=std::get_if<ApplyBoundaryConstraintChanges>(&geometry)) {
                    if (is_physical_wall_room_selection_geometry_review_command(geometry)) {
                        const Command inner{without_selection_completion(*constrained)};
                        if (is_physical_wall_room_profile_review_command(inner)) {
                            const auto inner_source=completed_boundary_constraint_entities(history,source,source_assets,
                                std::get<ApplyBoundaryConstraintChanges>(inner),retained_replay,active_policy,original_dimension_source);
                            validate_physical_wall_room_profile_review_source(source,inner_source,inner);
                        }
                    }
                    reviewed_source=completed_boundary_constraint_entities(history,source,source_assets,*constrained,retained_replay,active_policy,original_dimension_source);
                    validate_completed_constraint_change(source,reviewed_source,*constrained,retained_replay);
                    validate_physical_room_source_transition(source,reviewed_source,nullptr,constrained,active_policy);
                    if (boundary_constraint_assets(source_assets,*constrained)!=source_assets)
                        throw std::invalid_argument("Wall room review cannot change assets");
                } else {
                    const auto& ordinary=std::get<ApplyEntityChanges>(geometry);
                    reviewed_source=ordinary_entity_changes(source,ordinary.entity_changes);
                    validate_boundary_change(history,source,reviewed_source);
                    validate_constraint_change(source,reviewed_source);
                    validate_physical_room_source_transition(source,reviewed_source);
                }
                if (is_physical_wall_room_profile_review_command(geometry))
                    validate_physical_wall_room_profile_review_source(source,reviewed_source,geometry);
                const bool deletion=is_physical_wall_room_deletion_review_command(geometry);
                if (deletion) validate_physical_wall_room_deletion_review_source(source,reviewed_source,geometry,
                    command.room_review_geometry_proof,active_policy);
                (void)validate_state(reviewed_source,source_assets,active_policy);
                std::set<std::string> changed_walls,reviewed_rooms;
                for (const auto& [id,entity] : source) {
                    if (entity.type!="wall") continue;
                    const auto proposed=reviewed_source.find(id);
                    if (deletion && proposed==reviewed_source.end()) { changed_walls.insert(id);continue; }
                    if (proposed==reviewed_source.end() || proposed->second.type!="wall")
                        throw std::invalid_argument("Wall room review cannot remove or replace a physical source wall");
                    if (!exact_entity_payload(entity,proposed->second)) changed_walls.insert(id);
                }
                for (const auto& [id,entity] : reviewed_source)
                    if (entity.type=="wall" && (!source.contains(id) || source.at(id).type!="wall"))
                        throw std::invalid_argument("Wall room review cannot create a physical source wall");
                bool active_phase_scope=false;
                for (const auto& encoded : room_review_intents(command)) {
                    const auto intent=decode_physical_wall_room_review_intent(encoded);
                    active_phase_scope=active_phase_scope || intent.active_phase_room_scope;
                    for (const auto& decision : intent.retained) reviewed_rooms.insert(decision.room_id);
                }
                std::set<std::string> active_rooms;
                if (active_phase_scope) {
                    const auto ids=active_physical_wall_room_ids(source);
                    active_rooms.insert(ids.begin(),ids.end());
                }
                const auto organization=organize_project(source);
                for (const auto& [id,entity] : source) {
                    if (!is_physical_wall_room(entity)) continue;
                    if (active_phase_scope && !active_rooms.contains(id)) continue;
                    const auto context=organization.drawing_context(id);
                    if (!context || !context->complete())
                        throw std::invalid_argument("Wall room review requires resolved retained room contexts");
                    const auto lineage=validate_retained_physical_wall_room_lineage(entity,*context);
                    const bool affected=std::any_of(lineage.source_owner_ids.begin(),lineage.source_owner_ids.end(),
                        [&](const auto& wall) { return changed_walls.contains(wall); });
                    if (affected && !reviewed_rooms.contains(id))
                        throw std::invalid_argument("Wall room review must include every affected retained room in every context and plane");
                }
            }
            const auto replay=has_room_review_batch_completion(command) ?
                replay_physical_wall_room_review_batch(reviewed_source,room_review_intents(command),active_policy,
                    original_dimension_source ? original_dimension_source : &source) :
                replay_physical_wall_room_review(reviewed_source,command.room_review_intent,active_policy,
                    original_dimension_source ? original_dimension_source : &source);
            if (!phase_constraint_authoring_proofs(command).empty()) {
                // Geometry and the separately reviewed room suffix have
                // different authority. Compare the complete coordinated
                // geometry with its admitted stage, then replay room decisions
                // against that exact stage without granting physical edits.
                validate_phase_constraint_composed_originals(source,reviewed_source,command);
                const auto physical_owner = [](const Entity& entity) {
                    return entity.type == "wall" || entity.type == "opening" || entity.type == "corner_window" || entity.type == "wall_join" ||
                        entity.type == "roof" || entity.type == "roof_join" || entity.type == "slab" ||
                        entity.type == "column" || entity.type == "beam" || entity.type == "stair" ||
                        entity.type == "railing" || entity.type == "assembly_model" || entity.type == "assembly_instance";
                };
                for (const auto& [id, entity] : reviewed_source) {
                    if (!physical_owner(entity)) continue;
                    const auto retained = replay.entities.find(id);
                    if (retained == replay.entities.end() || !exact_entity_payload(entity, retained->second))
                        throw std::invalid_argument("Room review changed an admitted coordinated physical owner: " + id);
                }
                for (const auto& [id, entity] : replay.entities)
                    if (physical_owner(entity) && !reviewed_source.contains(id))
                        throw std::invalid_argument("Room review created an unreviewed coordinated physical owner: " + id);
            }
            validate_boundary_identity_transition(history,source,replay.entities);
            if (active_policy) {
                validate_active_design_preserved_dependents(source,replay.entities,false);
                (void)validate_active_phase_constraint_integrity(replay.entities);
            }
            else (void)validate_constraint_integrity(replay.entities);
            return replay.entities;
#else
            throw std::invalid_argument("Room review requires the production physical-wall engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    }
    if (has_joint_translation_completion(command)) {
        try {
            validate_joint_translation_mode(command, true);
            (void)command_to_json(Command{command});
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
            auto submitted = command;
            submitted.joint_translation.reset();
            submitted.joint_translation_completion = false;
            auto reconstructed = reconstruct_joint_translation(source, *command.joint_translation);
            reconstructed.expected_revision = command.expected_revision;
            reconstructed.message = command.message;
            std::vector<EntityChange> presentation_proof;
            if (joint_per_target_presentation(*command.joint_translation)) {
                validate_joint_presentation_proof(command);
                if (submitted.supplemental_entity_changes.size() != reconstructed.supplemental_entity_changes.size())
                    throw std::invalid_argument("Joint presentation proof differs from its reconstructed source targets");
                for (const auto& change : submitted.supplemental_entity_changes) {
                    const auto generated = std::find_if(reconstructed.supplemental_entity_changes.begin(),
                        reconstructed.supplemental_entity_changes.end(), [&](const auto& item) {
                            return item.kind == EntityChangeKind::upsert && item.entity.id == change.entity.id;
                        });
                    if (generated == reconstructed.supplemental_entity_changes.end() ||
                        !exact_entity_payload(generated->entity, change.entity))
                        throw std::invalid_argument("Joint presentation proof differs from its source-derived position-only consequence");
                }
                presentation_proof = reconstructed.supplemental_entity_changes;
                remove_joint_presentation_proof(submitted);
                remove_joint_presentation_proof(reconstructed);
            }
            for (const auto& change : submitted.supplemental_entity_changes) {
                auto generated = std::find_if(reconstructed.supplemental_entity_changes.begin(),
                    reconstructed.supplemental_entity_changes.end(), [&](const auto& item) {
                        return (item.kind == EntityChangeKind::upsert ? item.entity.id : item.entity_id) ==
                               (change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id);
                    });
                if (generated != reconstructed.supplemental_entity_changes.end() && generated->kind == change.kind &&
                    (change.kind == EntityChangeKind::upsert ? exact_entity_payload(generated->entity,change.entity) :
                                                             generated->entity_id == change.entity_id)) continue;
                if (change.kind != EntityChangeKind::upsert || !submitted.supplemental_source_completion)
                    throw std::invalid_argument("Joint presentation movement requires existing placement-only supplements");
                const auto found = source.find(change.entity.id);
                if (found == source.end() || found->second.type != change.entity.type ||
                    (change.entity.type != kAnnotationEntityType && change.entity.type != "reference_asset"))
                    throw std::invalid_argument("Joint presentation movement supports existing labels, symbols and references only");
                const auto& original = found->second;
                const auto& baseline = generated == reconstructed.supplemental_entity_changes.end() ? original : generated->entity;
                auto retained = change.entity;
                const auto view_offset = command.joint_translation->presentation_offset.value_or(command.joint_translation->offset);
                const auto check_position = [&](const nlohmann::json& previous, const nlohmann::json& proposed, Vec2 offset) {
                    if (!previous.is_array() || !proposed.is_array() || previous.size() != 2 || proposed.size() != 2)
                        throw std::invalid_argument("Joint presentation position is malformed");
                    for (std::size_t index = 0; index < 2; ++index) {
                        if (!previous[index].is_number() || !proposed[index].is_number())
                            throw std::invalid_argument("Joint presentation position must be numeric");
                        const double value = proposed[index].get<double>();
                        const double expected_value = previous[index].get<double>() + (index == 0 ? offset.x : offset.y);
                        if (!std::isfinite(value) || value != expected_value)
                            throw std::invalid_argument("Joint presentation position differs from its selected translation");
                    }
                };
                if (change.entity.type == "reference_asset") {
                    check_position(original.properties.at("position_m"),change.entity.properties.at("position_m"),view_offset);
                    retained.properties["position_m"] = baseline.properties.at("position_m");
                } else {
                    validate_annotation_entity(change.entity);
                    const auto& prior_state = original.properties.at("state");
                    auto& next_state = retained.properties.at("state");
                    for (const auto* kind : {"labels", "symbols"}) {
                        const auto& prior = prior_state.at(kind);
                        auto& next = next_state.at(kind);
                        const auto& generated_rows = baseline.properties.at("state").at(kind);
                        if (prior.size() != next.size() || generated_rows.size() != prior.size())
                            throw std::invalid_argument("Joint presentation movement cannot add, remove or reorder instances");
                        for (std::size_t index = 0; index < prior.size(); ++index) {
                            const auto& before = prior[index].at("placement");
                            auto& after = next[index].at("placement");
                            if (prior[index].at("id") != next[index].at("id"))
                                throw std::invalid_argument("Joint presentation movement must retain instance identities");
                            if (before.at("x") != after.at("x") || before.at("y") != after.at("y")) {
                                const auto offset = prior[index].value("model_plan",false)
                                    ? command.joint_translation->offset : view_offset;
                                check_position(nlohmann::json::array({before.at("x"),before.at("y")}),
                                               nlohmann::json::array({after.at("x"),after.at("y")}),offset);
                            }
                            after["x"] = generated_rows[index].at("placement").at("x");
                            after["y"] = generated_rows[index].at("placement").at("y");
                        }
                    }
                }
                if (!exact_entity_payload(retained,baseline))
                    throw std::invalid_argument("Joint presentation movement cannot change styling, contents, sizing or metadata");
                if (generated == reconstructed.supplemental_entity_changes.end())
                    reconstructed.supplemental_entity_changes.push_back(change);
                else *generated = change;
                reconstructed.supplemental_source_completion = true;
            }
            const bool rigid_joint = command.joint_translation->per_owner_rigid_completion || !command.joint_translation->owner_transformations.empty();
            const auto replay_source = rigid_joint ? joint_rigid_replay_source(source, *command.joint_translation) : source;
            auto expected = boundary_constraint_entities(replay_source, reconstructed, retained_replay, rigid_joint,
                rigid_joint ? &*command.joint_translation : nullptr);
            auto actual = boundary_constraint_entities(replay_source, submitted, retained_replay, rigid_joint,
                rigid_joint ? &*command.joint_translation : nullptr);
            if (actual.size() != expected.size())
                throw std::invalid_argument("Joint translation proof differs from its reconstructed source intent");
            for (const auto& [id, entity] : expected) {
                const auto found = actual.find(id);
                if (found == actual.end() || !exact_entity_payload(entity, found->second))
                    throw std::invalid_argument("Joint translation proof differs from its reconstructed source intent: " + id);
            }
            for (const auto& change : presentation_proof) {
                const auto original = source.find(change.entity.id);
                const auto baseline = actual.find(change.entity.id);
                if (original == source.end() || baseline == actual.end())
                    throw std::invalid_argument("Joint presentation proof overlaps an ordinary geometry edit");
                if (rigid_joint && change.entity.type == kAnnotationEntityType)
                    baseline->second = merge_selection_annotation_entities(original->second,change.entity,baseline->second,
                        AnnotationMergeMode::source_callout_geometry);
                else {
                    if (!exact_entity_payload(original->second,baseline->second))
                        throw std::invalid_argument("Joint presentation proof overlaps an ordinary geometry edit");
                    baseline->second = change.entity;
                }
            }
            if (rigid_joint) complete_joint_rigid_sources(source, actual, *command.joint_translation);
            if (rigid_joint) complete_joint_rigid_consequences(source, actual, *command.joint_translation);
            if (rigid_joint) complete_corner_window_geometry(source,actual);
            if (rigid_joint) validate_joint_rigid_topology(source, actual, *command.joint_translation);
            retain_joint_callout_placement(source, actual, *command.joint_translation);
            auto expected_relations = source;
            if (rigid_joint) complete_joint_rigid_consequences(source, expected_relations, *command.joint_translation,false);
            for (const auto& [id, entity] : source) {
                if (entity.type != "constraint") continue;
                const auto found = actual.find(id);
                if (found == actual.end() || !exact_entity_payload(expected_relations.at(id), found->second))
                    throw std::invalid_argument("Joint translation cannot change a persisted relation: " + id);
            }
            validate_boundary_identity_transition(history, source, actual);
            return actual;
#else
            throw std::invalid_argument("Joint translation requires the production constraint engine");
#endif
        } catch (const DocumentError&) { throw; }
        catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    }
    if (!has_rigid_group_completion(command))
        return boundary_constraint_entities(source, command, retained_replay);
    try {
        validate_rigid_group_intent(command, true);
        (void)command_to_json(Command{command});
        auto partial = command;
        partial.rigid_group_transform.reset(); partial.rigid_group_completion = false;
        const auto& rigid = *command.rigid_group_transform;

        // Dependency closure is ownership, even when its payload is unchanged.
        // This includes source archives, deductions, analytical callouts and
        // all hard-connected owners; no lane may borrow the other's authority.
        using Ids = std::set<std::string, std::less<>>;
        std::map<std::string, Ids, std::less<>> links;
        const auto link = [&](const std::string& first, const std::string& second) {
            links[first].insert(second); links[second].insert(first);
        };
        const auto reserve_dependencies = [&](const Entity& entity) {
            const auto& id = entity.id;
            if (entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
                for (const auto& binding : decoded.constraint->bindings) link(id, binding.owner_id);
            }
            if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                const auto target = entity.properties.find("target");
                if (target != entity.properties.end() && target->is_object() && target->contains("entity_id") &&
                    target->at("entity_id").is_string()) link(id, target->at("entity_id").get<std::string>());
            }
            if (can_recognize_boundary_entity_type(entity.type)) {
                if (entity.properties.contains("wall_measurement_source"))
                    for (const auto& wall : exterior_wall_measurement_source_ids(entity)) link(id, wall);
                if (const auto deductions = entity.properties.find("deduction_ids"); deductions != entity.properties.end()) {
                    if (!deductions->is_array()) throw std::invalid_argument("Measured deductions must be an array");
                    for (const auto& child : *deductions) {
                        if (!child.is_string()) throw std::invalid_argument("Measured deduction identifiers are invalid");
                        link(id, child.get<std::string>());
                    }
                }
                const auto source_uses = [&](const auto& self, const nlohmann::json& value) -> void {
                    if (value.is_object()) {
                        if (value.contains("owner_id") && value.at("owner_id").is_string())
                            link(id, value.at("owner_id").get<std::string>());
                        for (const auto& item : value.items()) self(self, item.value());
                    } else if (value.is_array()) for (const auto& item : value) self(self, item);
                };
                for (const auto* key : {"measurement_linework_sources", "measurement_linework_group"})
                    if (entity.extensions.contains(key)) source_uses(source_uses, entity.extensions.at(key));
            }
            if (entity.type == "opening" && entity.properties.contains("wall_id") && entity.properties.at("wall_id").is_string())
                link(id, entity.properties.at("wall_id").get<std::string>());
        };
        for (const auto& [id, entity] : source) { (void)id; reserve_dependencies(entity); }
        for (const auto* lane : std::initializer_list<const std::vector<EntityChange>*>{
                &rigid.entity_changes, &partial.entity_changes, &partial.physical_entity_changes,
                &partial.supplemental_entity_changes})
            for (const auto& change : *lane)
                if (change.kind == EntityChangeKind::upsert) reserve_dependencies(change.entity);
        const auto closure = [&](Ids ids) {
            std::vector<std::string> pending(ids.begin(), ids.end());
            for (std::size_t index = 0; index < pending.size(); ++index)
                if (const auto found = links.find(pending[index]); found != links.end())
                    for (const auto& id : found->second)
                        if (ids.insert(id).second) pending.push_back(id);
            return ids;
        };
        const auto change_id = [](const EntityChange& change) -> const std::string& {
            return change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id;
        };
        Ids rigid_seeds, partial_seeds;
        for (const auto& transform : rigid.transformations) rigid_seeds.insert(transform.boundary_id);
        for (const auto& change : rigid.entity_changes) rigid_seeds.insert(change_id(change));
        for (const auto& edit : partial.boundary_edits) partial_seeds.insert(edit.boundary_id);
        for (const auto& edit : partial.wall_edits) partial_seeds.insert(edit.wall_id);
        for (const auto& edit : partial.exterior_source_edits) partial_seeds.insert(edit.boundary_id);
        for (const auto& edit : partial.measured_stroke_edits) partial_seeds.insert(edit.stroke_id);
        for (const auto& move : partial.dimension_placement_moves) partial_seeds.insert(move.dimension_id);
        const auto rigid_scope = closure(std::move(rigid_seeds));
        const auto partial_scope = closure(std::move(partial_seeds));
        for (const auto& id : partial_scope)
            if (rigid_scope.contains(id))
                throw std::invalid_argument("Rigid and connected lanes share an ownership component: " + id);
        for (const auto* lane : {&partial.entity_changes, &partial.physical_entity_changes, &partial.supplemental_entity_changes})
            for (const auto& change : *lane)
                if (rigid_scope.contains(change_id(change)))
                    throw std::invalid_argument("Connected supplement overlaps rigid ownership: " + change_id(change));

        auto rigid_result = boundary_transform_entities(history, source, rigid);
        // The new composition dialect preserves rigid callout placement even
        // when canonical exterior reconciliation reflows an automatic label.
        // Older standalone transform replay keeps its historical semantics.
        std::set<std::string,std::less<>> rigid_owners;
        for(const auto& transform:rigid.transformations)rigid_owners.insert(transform.boundary_id);
        const auto moved_callouts=rigid_boundary_targets(source,rigid);
        for(const auto& [id,entity]:moved_callouts) {
            if(!can_recognize_boundary_dimension_entity_type(entity.type))continue;
            const auto decoded=decode_boundary_dimension_entity(entity);
            if(!decoded.supported() || !rigid_owners.contains(decoded.dimension->boundary_id))continue;
            if(!rigid_result.contains(id))throw std::invalid_argument("Rigid source reconciliation retired an attached callout");
            const auto current=decode_boundary_dimension_entity(rigid_result.at(id));
            if(!current.supported() || current.dimension->boundary_id!=decoded.dimension->boundary_id ||
                current.dimension->kind!=decoded.dimension->kind || current.dimension->segment_id!=decoded.dimension->segment_id ||
                current.dimension->vertex_id!=decoded.dimension->vertex_id || current.dimension->secondary_segment_id!=decoded.dimension->secondary_segment_id ||
                current.dimension->segment_chain_ids!=decoded.dimension->segment_chain_ids ||
                current.dimension->corner_leg!=decoded.dimension->corner_leg)
                throw std::invalid_argument("Rigid source reconciliation changed a saved analytical target");
            if (decoded.dimension->kind==BoundaryDimensionKind::corner_window_leg_length)
                (void)decoded.dimension->resolve(rigid_result);
            else (void)decoded.dimension->resolve(rigid_result.at(decoded.dimension->boundary_id));
            rigid_result.at(id)=entity;
        }
        const auto partial_result = boundary_constraint_entities(source, partial, retained_replay);
        // Mixed selection never makes an existing fixed anchor movable.
        for (const auto& [id, entity] : source) {
            if (entity.type != "constraint") continue;
            const auto decoded = decode_constraint_entity(entity);
            if (decoded.supported() && decoded.constraint->relation == ConstraintRelationKind::fixed_anchor &&
                (rigid_result.at(id) != entity || !partial_result.contains(id) || partial_result.at(id) != entity))
                throw std::invalid_argument("Mixed geometry cannot rewrite an existing fixed anchor");
        }
        validate_constraint_change(source, rigid_result, false, true, true);
        validate_completed_constraint_change(source, partial_result, partial, retained_replay);

        auto result = source;
        Ids touched;
        const auto merge = [&](const auto& replay, const Ids& forbidden) {
            for (const auto& [id, entity] : source) {
                const auto after = replay.find(id);
                if (after != replay.end() && exact_entity_payload(entity, after->second)) continue;
                if (forbidden.contains(id) || !touched.insert(id).second)
                    throw std::invalid_argument("Mixed replay delta overlaps reserved ownership: " + id);
                if (after == replay.end()) result.erase(id);
                else result.at(id) = after->second;
            }
            for (const auto& [id, entity] : replay) {
                if (source.contains(id)) continue;
                if (forbidden.contains(id) || !touched.insert(id).second)
                    throw std::invalid_argument("Mixed replay creation overlaps reserved ownership: " + id);
                result.emplace(id, entity);
            }
        };
        merge(rigid_result, partial_scope); merge(partial_result, rigid_scope);
        Ids rigid_wall_ids;
        for (const auto& id : rigid_scope)
            if (source.contains(id) && source.at(id).type == "wall") rigid_wall_ids.insert(id);
        for (const auto& edit : partial.wall_edits) if (edit.version == 4 || edit.version == 5) rigid_wall_ids.insert(edit.wall_id);
        (void)validate_constraint_integrity(result);
        validate_constraint_transition(source, result, false, rigid_wall_ids);
        validate_constraint_edit_topology(source, result, rigid_wall_ids);
        validate_boundary_identity_transition(history, source, result);
        return result;
    } catch (const DocumentError&) { throw; }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
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

nlohmann::json command_transform_to_json(const PlanarTransform& transform);
PlanarTransform command_transform_from_json(const nlohmann::json& value);

nlohmann::json joint_translation_to_json(const JointTranslationIntent& intent) {
    const bool rigid = intent.per_owner_rigid_completion || !intent.owner_transformations.empty();
    if (!std::isfinite(intent.offset.x) || !std::isfinite(intent.offset.y) ||
        (!rigid && intent.offset.x == 0.0 && intent.offset.y == 0.0))
        throw std::invalid_argument("Joint translation requires a finite nonzero offset");
    if (intent.presentation_offset && (!std::isfinite(intent.presentation_offset->x) || !std::isfinite(intent.presentation_offset->y)))
        throw std::invalid_argument("Joint presentation offset must be finite");
    std::set<std::string, std::less<>> selected;
    const auto validate_ids = [&](const std::vector<std::string>& ids) {
        for (const auto& id : ids)
            if (!is_valid_identifier(id) || !selected.insert(id).second)
                throw std::invalid_argument("Joint translation selected IDs are invalid or repeated");
    };
    validate_ids(intent.rigid_boundary_ids);
    validate_ids(intent.rigid_stroke_ids);
    validate_ids(intent.partial_wall_ids);
    validate_ids(intent.dimension_ids);
    if (rigid && (intent.per_owner_translation_completion || !intent.owner_translations.empty()))
        throw std::invalid_argument("Rigid joint intent cannot combine translation geometry authority");
    const bool per_owner = rigid || intent.per_owner_translation_completion ||
        !intent.owner_translations.empty() || !intent.dimension_translations.empty();
    const bool empty_geometry = intent.partial_wall_ids.empty() &&
        intent.rigid_boundary_ids.empty() && intent.rigid_stroke_ids.empty();
    if (selected.size() > 4096 || (per_owner ? empty_geometry :
        intent.partial_wall_ids.empty() || (intent.rigid_boundary_ids.empty() && intent.rigid_stroke_ids.empty())))
        throw std::invalid_argument("Joint translation requires a bounded supported geometry selection");
    const bool per_target = joint_per_target_presentation(intent);
    if (per_target && intent.presentation_offset)
        throw std::invalid_argument("Per-target joint presentation cannot combine a legacy shared presentation offset");
    if (intent.annotation_translations.size() > 1000 ||
        intent.reference_translations.size() > 1000 - intent.annotation_translations.size())
        throw std::invalid_argument("Joint presentation targets exceed their aggregate budget");
    const auto encode_offsets = [&](const std::vector<JointOwnerTranslationIntent>& targets,
        const std::set<std::string, std::less<>>& expected) {
        if (targets.size() != expected.size() || targets.size() > 4096)
            throw std::invalid_argument("Joint owner offsets require exact selected target coverage");
        auto encoded_targets = nlohmann::json::array();
        std::set<std::string, std::less<>> seen;
        for (const auto& target : targets) {
            if (!is_valid_identifier(target.owner_id) || !expected.contains(target.owner_id) ||
                !seen.insert(target.owner_id).second || !std::isfinite(target.offset.x) || !std::isfinite(target.offset.y))
                throw std::invalid_argument("Joint owner offsets require unique selected identities and finite translations");
            encoded_targets.push_back({{"owner_id", target.owner_id}, {"offset", command_vec2_to_json(target.offset)}});
        }
        return encoded_targets;
    };
    auto owners = nlohmann::json::array(), dimensions = nlohmann::json::array();
    auto transformations = nlohmann::json::array();
    if (per_owner) {
        std::set<std::string, std::less<>> geometry(intent.rigid_boundary_ids.begin(), intent.rigid_boundary_ids.end());
        geometry.insert(intent.rigid_stroke_ids.begin(), intent.rigid_stroke_ids.end());
        geometry.insert(intent.partial_wall_ids.begin(), intent.partial_wall_ids.end());
        if (rigid) {
            if (intent.owner_transformations.size() != geometry.size())
                throw std::invalid_argument("Rigid joint operators require exact selected target coverage");
            std::set<std::string, std::less<>> seen;
            for (const auto& target : intent.owner_transformations) {
                validate_rigid_owner_transform(target);
                if (!geometry.contains(target.owner_id) || !seen.insert(target.owner_id).second)
                    throw std::invalid_argument("Rigid joint operators require unique selected owners");
                transformations.push_back({{"owner_id", target.owner_id}, {"transform", command_transform_to_json(target.transform)}});
            }
        } else owners = encode_offsets(intent.owner_translations, geometry);
        dimensions = encode_offsets(intent.dimension_translations,
            std::set<std::string, std::less<>>(intent.dimension_ids.begin(), intent.dimension_ids.end()));
    }
    auto annotations = nlohmann::json::array();
    auto references = nlohmann::json::array();
    std::set<std::pair<std::string, std::string>> children;
    for (const auto& target : intent.annotation_translations) {
        if (!is_valid_identifier(target.owner_id) || target.child_id.empty() || target.child_id.size() > 256 ||
            !is_valid_utf8_without_nul(target.child_id) || selected.contains(target.owner_id) ||
            !children.emplace(target.owner_id, target.child_id).second ||
            !std::isfinite(target.offset.x) || !std::isfinite(target.offset.y))
            throw std::invalid_argument("Joint annotation targets require unique bounded source identities");
        annotations.push_back({{"owner_id", target.owner_id}, {"child_id", target.child_id},
            {"offset", command_vec2_to_json(target.offset)}});
    }
    std::set<std::string, std::less<>> reference_ids;
    for (const auto& target : intent.reference_translations) {
        if (!is_valid_identifier(target.reference_id) || selected.contains(target.reference_id) ||
            !reference_ids.insert(target.reference_id).second ||
            !std::isfinite(target.offset.x) || !std::isfinite(target.offset.y))
            throw std::invalid_argument("Joint reference targets require unique bounded source identities");
        if (std::any_of(intent.annotation_translations.begin(), intent.annotation_translations.end(),
            [&](const auto& child) { return child.owner_id == target.reference_id; }))
            throw std::invalid_argument("Joint reference target aliases an annotation owner");
        references.push_back({{"reference_id", target.reference_id}, {"offset", command_vec2_to_json(target.offset)}});
    }
    auto encoded = nlohmann::json{{"version", rigid ? 4 : per_owner ? 3 : per_target ? 2 : 1}, {"offset", command_vec2_to_json(intent.offset)},
        {"rigid_boundary_ids", intent.rigid_boundary_ids}, {"rigid_stroke_ids", intent.rigid_stroke_ids},
        {"partial_wall_ids", intent.partial_wall_ids}, {"move_connected_objects", intent.move_connected_objects},
        {"dimension_ids", intent.dimension_ids},
        {"presentation_offset", intent.presentation_offset ? command_vec2_to_json(*intent.presentation_offset) : nlohmann::json(nullptr)}};
    if (per_target) {
        encoded["annotation_translations"] = std::move(annotations);
        encoded["reference_translations"] = std::move(references);
    }
    if (per_owner) {
        if (rigid) {
            encoded["per_owner_rigid_completion"] = true;
            encoded["owner_transformations"] = std::move(transformations);
        } else encoded["owner_translations"] = std::move(owners);
        encoded["dimension_translations"] = std::move(dimensions);
    }
    if (rigid && encoded.dump().size() > 1024 * 1024)
        throw std::invalid_argument("Rigid joint intent exceeds its proof budget");
    if (intent.physical_room_dimension_completion) {
        if (intent.dimension_ids.empty())
            throw std::invalid_argument("Current physical-room callout completion requires selected dimensions");
        encoded["version"] = encoded.at("version").get<int>() + 4;
        encoded["physical_room_dimension_completion"] = true;
        if (encoded.dump().size() > 1024 * 1024)
            throw std::invalid_argument("Current physical-room joint intent exceeds its proof budget");
    }
    return encoded;
}

JointTranslationIntent joint_translation_from_json(const nlohmann::json& value) {
    if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
        value.at("version") >= 5 && value.at("version") <= 8) {
        const auto marker = value.find("physical_room_dimension_completion");
        if (marker == value.end() || !marker->is_boolean() || !marker->get<bool>() ||
            value.dump().size() > 1024 * 1024)
            throw std::invalid_argument("Current physical-room joint intent requires its explicit bounded marker");
        auto base = value;
        base.erase("physical_room_dimension_completion");
        base["version"] = value.at("version").get<int>() - 4;
        // Decode the exact older field shape without changing that dialect's
        // meaning. The new source admission is restored explicitly afterward.
        auto result = joint_translation_from_json(base);
        result.physical_room_dimension_completion = true;
        (void)joint_translation_to_json(result);
        return result;
    }
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4))
        throw std::invalid_argument("Unsupported joint translation intent version");
    const bool rigid = value.at("version") == 4;
    const bool per_owner = rigid || value.at("version") == 3;
    const bool per_target = per_owner || value.at("version") == 2;
    if (rigid) {
        command_exact_fields(value, {"version", "offset", "rigid_boundary_ids", "rigid_stroke_ids",
            "partial_wall_ids", "move_connected_objects", "dimension_ids", "presentation_offset",
            "annotation_translations", "reference_translations", "owner_transformations", "dimension_translations",
            "per_owner_rigid_completion"}, DocumentErrorCode::invalid_entity, "rigid joint intent");
        if (!value.at("per_owner_rigid_completion").is_boolean() ||
            !value.at("per_owner_rigid_completion").get<bool>() || value.dump().size() > 1024 * 1024)
            throw std::invalid_argument("Rigid joint completion requires its explicit bounded marker");
    } else if (per_owner)
        command_exact_fields(value, {"version", "offset", "rigid_boundary_ids", "rigid_stroke_ids",
            "partial_wall_ids", "move_connected_objects", "dimension_ids", "presentation_offset",
            "annotation_translations", "reference_translations", "owner_translations", "dimension_translations"},
            DocumentErrorCode::invalid_entity, "joint translation intent");
    else if (per_target)
        command_exact_fields(value, {"version", "offset", "rigid_boundary_ids", "rigid_stroke_ids",
            "partial_wall_ids", "move_connected_objects", "dimension_ids", "presentation_offset",
            "annotation_translations", "reference_translations"}, DocumentErrorCode::invalid_entity, "joint translation intent");
    else
        command_exact_fields(value, {"version", "offset", "rigid_boundary_ids", "rigid_stroke_ids",
            "partial_wall_ids", "move_connected_objects", "dimension_ids", "presentation_offset"}, DocumentErrorCode::invalid_entity, "joint translation intent");
    if (!value.at("move_connected_objects").is_boolean())
        throw std::invalid_argument("Unsupported joint translation intent or movement flag");
    JointTranslationIntent result;
    result.per_target_presentation_completion = per_target;
    result.per_owner_translation_completion = per_owner && !rigid;
    result.per_owner_rigid_completion = rigid;
    result.offset = command_vec2_from_json(value.at("offset"), "joint translation offset");
    if (!value.at("presentation_offset").is_null())
        result.presentation_offset = command_vec2_from_json(value.at("presentation_offset"), "joint presentation offset");
    const auto read_ids = [&](const char* key, std::vector<std::string>& ids) {
        const auto& values = value.at(key);
        if (!values.is_array() || values.size() > 4096)
            throw std::invalid_argument("Joint translation selected IDs must be a bounded array");
        for (const auto& id : values) ids.push_back(command_string(id, key, kMaximumIdBytes));
    };
    read_ids("rigid_boundary_ids", result.rigid_boundary_ids);
    read_ids("rigid_stroke_ids", result.rigid_stroke_ids);
    read_ids("partial_wall_ids", result.partial_wall_ids);
    read_ids("dimension_ids", result.dimension_ids);
    if (per_owner) {
        const auto read_offsets = [&](const char* key, std::vector<JointOwnerTranslationIntent>& targets) {
            const auto& values = value.at(key);
            if (!values.is_array() || values.size() > 4096)
                throw std::invalid_argument("Joint owner offsets must be bounded arrays");
            for (const auto& target : values) {
                command_exact_fields(target, {"owner_id", "offset"}, DocumentErrorCode::invalid_entity, "joint owner translation");
                targets.push_back({command_string(target.at("owner_id"), "joint translation owner", kMaximumIdBytes),
                    command_vec2_from_json(target.at("offset"), "joint owner offset")});
            }
        };
        if (rigid) {
            const auto& targets = value.at("owner_transformations");
            if (!targets.is_array() || targets.size() > 4096)
                throw std::invalid_argument("Rigid joint operators must be a bounded array");
            for (const auto& target : targets) {
                command_exact_fields(target, {"owner_id", "transform"}, DocumentErrorCode::invalid_entity, "rigid joint owner");
                result.owner_transformations.push_back({command_string(target.at("owner_id"), "rigid joint owner", kMaximumIdBytes),
                    command_transform_from_json(target.at("transform"))});
            }
        } else read_offsets("owner_translations", result.owner_translations);
        read_offsets("dimension_translations", result.dimension_translations);
    }
    if (per_target) {
        const auto& annotations = value.at("annotation_translations");
        const auto& references = value.at("reference_translations");
        if (!annotations.is_array() || !references.is_array() || annotations.size() > 1000 ||
            references.size() > 1000 - annotations.size())
            throw std::invalid_argument("Joint presentation targets must be bounded arrays");
        for (const auto& target : annotations) {
            command_exact_fields(target, {"owner_id", "child_id", "offset"},
                DocumentErrorCode::invalid_entity, "joint annotation translation");
            result.annotation_translations.push_back({command_string(target.at("owner_id"), "joint annotation owner", kMaximumIdBytes),
                command_string(target.at("child_id"), "joint annotation child", 256),
                command_vec2_from_json(target.at("offset"), "joint annotation offset")});
        }
        for (const auto& target : references) {
            command_exact_fields(target, {"reference_id", "offset"},
                DocumentErrorCode::invalid_entity, "joint reference translation");
            result.reference_translations.push_back({command_string(target.at("reference_id"), "joint reference owner", kMaximumIdBytes),
                command_vec2_from_json(target.at("offset"), "joint reference offset")});
        }
    }
    result.move_connected_objects = value.at("move_connected_objects").get<bool>();
    (void)joint_translation_to_json(result);
    return result;
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

JointTranslationOffsets resolve_joint_translation_offsets(
    const std::map<std::string, Entity, std::less<>>& source, const JointTranslationIntent& intent) {
    (void)joint_translation_to_json(intent);
    const bool rigid = intent.per_owner_rigid_completion || !intent.owner_transformations.empty();
    const bool per_owner = rigid || intent.per_owner_translation_completion ||
        !intent.owner_translations.empty() || !intent.dimension_translations.empty();
    JointTranslationOffsets result;
    if (per_owner) {
        for (const auto& target : intent.owner_translations) result.owner_offsets.emplace(target.owner_id, target.offset);
        for (const auto& target : intent.dimension_translations) result.dimension_offsets.emplace(target.owner_id, target.offset);
        for (const auto& target : intent.owner_transformations) result.owner_transforms.emplace(target.owner_id, target.transform);
    }
    const auto require = [&](const std::string& id) -> const Entity& {
        const auto found = source.find(id);
        if (found == source.end()) throw std::invalid_argument("Joint translation source target does not exist: " + id);
        return found->second;
    };
    std::set<std::string,std::less<>> semantic_available;
    std::optional<std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>> source_checks;
    if (rigid) {
        for (const auto& [id, entity] : source) { (void)entity; semantic_available.insert(id); }
        for (const auto& [id, entity] : source) {
            (void)id;
            if (entity.type != "model_phases") continue;
            const auto phases = ModelPhases::from_json(entity.properties.at("model"));
            const auto active = phases.active_state();
            for (const auto& member : phases.entity_ids())
                if (!active.contains(member) || active.at(member) == ModelPhase::demolished) semantic_available.erase(member);
        }
    }
    for (const auto& id : intent.rigid_boundary_ids) {
        const auto& entity = require(id);
        if (!can_recognize_boundary_entity_type(entity.type))
            throw std::invalid_argument("Joint translation boundary source has the wrong owner type");
        (void)decode_identified_boundary_entity(entity);
        if (rigid) {
            if (!semantic_available.contains(id) || entity.extensions.contains("physical_wall_room"))
                throw std::invalid_argument("Rigid joint selected boundary is unavailable or requires reviewed physical-room repair");
            if (entity.properties.contains("wall_measurement_source") && !wall_measurement_source_current(source,entity))
                throw std::invalid_argument("Rigid joint selected physical measured boundary is stale");
            if (entity.extensions.contains("measurement_linework_sources") || entity.extensions.contains("measurement_linework_group")) {
                if (!source_checks) source_checks = measurement_linework_source_checks(source,&semantic_available);
                if (!measurement_linework_source_current(*source_checks,entity))
                    throw std::invalid_argument("Rigid joint selected measured boundary requires current semantic source lineage");
            }
            if (const auto deductions = entity.properties.find("deduction_ids"); deductions != entity.properties.end()) {
                if (!deductions->is_array()) throw std::invalid_argument("Rigid joint deductions must be a source ID array");
                std::set<std::string,std::less<>> seen;
                for (const auto& child : *deductions) {
                    if (!child.is_string()) throw std::invalid_argument("Rigid joint deduction ID is malformed");
                    const auto child_id = child.get<std::string>();
                    if (child_id == id || !seen.insert(child_id).second ||
                        std::find(intent.rigid_boundary_ids.begin(),intent.rigid_boundary_ids.end(),child_id) == intent.rigid_boundary_ids.end() ||
                        !(result.owner_transforms.at(id) == result.owner_transforms.at(child_id)))
                        throw std::invalid_argument("Rigid joint deductions require complete compatible selected operators");
                }
            }
        }
        if (!per_owner) result.owner_offsets.emplace(id, intent.offset);
    }
    for (const auto& id : intent.rigid_stroke_ids) {
        const auto& entity = require(id);
        if (entity.type != "measurement_linework")
            throw std::invalid_argument("Joint translation stroke source has the wrong owner type");
        const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
        if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
        if (rigid && !semantic_available.contains(id)) throw std::invalid_argument("Rigid joint stroke is unavailable in its semantic phase");
        if (!per_owner) result.owner_offsets.emplace(id, intent.offset);
    }
    for (const auto& id : intent.partial_wall_ids) {
        const auto& entity = require(id);
        if (entity.type != "wall") throw std::invalid_argument("Joint translation wall source has the wrong owner type");
        validate_entity(entity);
        if (rigid && !semantic_available.contains(id)) throw std::invalid_argument("Rigid joint wall is unavailable in its semantic phase");
        if (!per_owner) result.owner_offsets.emplace(id, intent.offset);
    }
    bool current_room_callout = false;
    for (const auto& id : intent.dimension_ids) {
        const auto& entity = require(id);
        if (!can_recognize_boundary_dimension_entity_type(entity.type))
            throw std::invalid_argument("Joint translation dimension source has the wrong owner type");
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
        const auto& owner_entity = require(decoded.dimension->boundary_id);
        if (intent.physical_room_dimension_completion) {
            (void)resolve_current_boundary_dimension(*decoded.dimension, source);
            current_room_callout = current_room_callout || is_physical_wall_room(owner_entity);
        } else if (decoded.dimension->kind==BoundaryDimensionKind::corner_window_leg_length)
            (void)resolve_current_boundary_dimension(*decoded.dimension,source);
        else (void)decoded.dimension->resolve(owner_entity);
        if (!per_owner) result.dimension_offsets.emplace(id, intent.offset);
        if (per_owner) {
            const auto owner = result.owner_offsets.find(decoded.dimension->boundary_id);
            const bool selected_owner = std::find(intent.rigid_boundary_ids.begin(), intent.rigid_boundary_ids.end(), decoded.dimension->boundary_id) != intent.rigid_boundary_ids.end() ||
                std::find(intent.rigid_stroke_ids.begin(), intent.rigid_stroke_ids.end(), decoded.dimension->boundary_id) != intent.rigid_stroke_ids.end() ||
                std::find(intent.partial_wall_ids.begin(), intent.partial_wall_ids.end(), decoded.dimension->boundary_id) != intent.partial_wall_ids.end();
            const auto selected = result.dimension_offsets.at(id);
            if (rigid && selected_owner) {
                const auto before = decoded.dimension->text_position;
                const auto after = transform_point(before, result.owner_transforms.at(decoded.dimension->boundary_id));
                if (after.x != before.x + selected.x || after.y != before.y + selected.y)
                    throw std::invalid_argument("Joint selected callout contradicts its captured rigid owner operator");
            } else if (selected_owner && owner != result.owner_offsets.end() &&
                (owner->second.x != selected.x || owner->second.y != selected.y))
                throw std::invalid_argument("Joint selected callout contradicts its selected rigid owner's translation");
        }
    }
    if (intent.physical_room_dimension_completion && !current_room_callout)
        throw std::invalid_argument("Current physical-room joint intent must select an actual current room callout");
    return result;
}

Entity merge_selection_annotation_entities(const Entity& original, const Entity& geometry, const Entity& ordinary,
    AnnotationMergeMode mode) {
    if (mode != AnnotationMergeMode::existing_rows && mode != AnnotationMergeMode::source_callout_geometry &&
        mode != AnnotationMergeMode::source_callout_proof)
        throw std::invalid_argument("Annotation merge mode is unsupported");
    if (original.type != kAnnotationEntityType || geometry.type != original.type || ordinary.type != original.type ||
        geometry.id != original.id || ordinary.id != original.id ||
        geometry.required != original.required || ordinary.required != original.required)
        throw std::invalid_argument("Annotation merge requires one original owner identity and type");
    validate_annotation_entity(original);
    validate_annotation_entity(geometry);
    validate_annotation_entity(ordinary);
    auto geometry_prefix = geometry;
    auto ordinary_prefix = ordinary;
    auto callout_suffix = nlohmann::json::array();
    if (mode != AnnotationMergeMode::existing_rows) {
        const auto& before = original.properties.at("state").at("overrides");
        auto& first = geometry_prefix.properties.at("state").at("overrides");
        auto& second = ordinary_prefix.properties.at("state").at("overrides");
        if (first.size() < before.size() || first.size()-before.size() > 8192 || second.size() < before.size())
            throw std::invalid_argument("Source callout merge cannot remove or unboundedly extend saved rows");
        std::set<std::pair<std::string,std::string>> identities;
        for (const auto& row : before)
            identities.emplace(row.at("target_id").get<std::string>(),row.at("target_kind").get<std::string>());
        for (std::size_t index=before.size(); index<first.size(); ++index) {
            const auto& row = first.at(index);
            const auto id = row.at("target_id").get<std::string>();
            const auto kind = row.at("target_kind").get<std::string>();
            if ((kind != "area_name" && kind != "area_calculation") ||
                !identities.contains({id,kind == "area_name" ? "area_calculation" : "area_name"}) ||
                !identities.emplace(id,kind).second)
                throw std::invalid_argument("Source callout merge requires a missing separated counterpart");
            callout_suffix.push_back(row);
        }
        if (second.size() != before.size()) {
            if (mode != AnnotationMergeMode::source_callout_proof || second.size() != first.size())
                throw std::invalid_argument("Ordinary selection cannot add annotation rows");
            for (std::size_t index=before.size(); index<second.size(); ++index)
                if (second.at(index).dump() != first.at(index).dump())
                    throw std::invalid_argument("Callout proof differs from its reconstructed suffix");
        }
        first.erase(first.begin()+static_cast<std::ptrdiff_t>(before.size()),first.end());
        second.erase(second.begin()+static_cast<std::ptrdiff_t>(before.size()),second.end());
        auto& first_version = geometry_prefix.properties.at("state").at("version");
        auto& second_version = ordinary_prefix.properties.at("state").at("version");
        const auto original_version = original.properties.at("state").at("version").get<int>();
        if (first_version.get<int>() < original_version || second_version.get<int>() < original_version)
            throw std::invalid_argument("Source callout merge cannot downgrade its annotation schema");
        // Independent symbol and callout edits may require different known
        // schema floors. Their payload fields still undergo the ordinary join.
        const auto version = std::max(first_version.get<int>(),second_version.get<int>());
        first_version = second_version = version;
    }
    for (const auto* table : {"labels", "symbols", "overrides"}) {
        const auto& before = original.properties.at("state").at(table);
        const auto& first = geometry_prefix.properties.at("state").at(table);
        const auto& second = ordinary_prefix.properties.at("state").at(table);
        if (first.size() != before.size() || second.size() != before.size())
            throw std::invalid_argument("Annotation merge must preserve saved row inventories");
        for (std::size_t index=0; index<before.size(); ++index)
            for (const auto* key : {"id", "target_id", "target_kind"})
                if (before.at(index).contains(key) && (!first.at(index).contains(key) || !second.at(index).contains(key) ||
                    first.at(index).at(key) != before.at(index).at(key) || second.at(index).at(key) != before.at(index).at(key)))
                    throw std::invalid_argument("Annotation merge must preserve saved row identities and order");
    }
    // Optional values distinguish missing keys from a present JSON null.
    const auto merge = [&](const auto& self, const std::optional<nlohmann::json>& before,
        const std::optional<nlohmann::json>& first, const std::optional<nlohmann::json>& second,
        std::size_t depth) -> std::optional<nlohmann::json> {
        if (depth > 64) throw std::invalid_argument("Annotation merge exceeds its nesting budget");
        if (first == second || second == before) return first;
        if (first == before) return second;
        if (!before || !first || !second) throw std::invalid_argument("Annotation lanes change the same saved field");
        if (before->is_object() && first->is_object() && second->is_object()) {
            std::set<std::string, std::less<>> keys;
            for (const auto* value : {&*before, &*first, &*second})
                for (const auto& item : value->items()) keys.insert(item.key());
            auto result = nlohmann::json::object();
            for (const auto& key : keys) {
                const auto read = [&](const auto& value) -> std::optional<nlohmann::json> {
                    return value.contains(key) ? std::optional<nlohmann::json>{value.at(key)} : std::nullopt;
                };
                const auto next = self(self,read(*before),read(*first),read(*second),depth+1);
                if (next) result[key] = *next;
            }
            return result;
        }
        if (before->is_array() && first->is_array() && second->is_array() &&
            before->size() == first->size() && before->size() == second->size()) {
            auto result = nlohmann::json::array();
            for (std::size_t index=0; index<before->size(); ++index) {
                const auto& row = before->at(index);
                if (row.is_object()) for (const auto* key : {"id", "target_id", "target_kind"})
                    if (row.contains(key) && (!first->at(index).is_object() || !second->at(index).is_object() ||
                        !first->at(index).contains(key) || !second->at(index).contains(key) ||
                        first->at(index).at(key) != row.at(key) || second->at(index).at(key) != row.at(key)))
                        throw std::invalid_argument("Annotation merge must preserve saved row identities and order");
                const auto next = self(self,row,first->at(index),second->at(index),depth+1);
                if (!next) throw std::invalid_argument("Annotation merge cannot remove an array position");
                result.push_back(*next);
            }
            return result;
        }
        throw std::invalid_argument("Annotation lanes change the same saved field");
    };
    if (original.properties.dump().size() > 1024*1024 || geometry.properties.dump().size() > 1024*1024 ||
        ordinary.properties.dump().size() > 1024*1024)
        throw std::invalid_argument("Annotation merge exceeds its payload budget");
    auto result = original;
    result.properties = *merge(merge,original.properties,geometry_prefix.properties,ordinary_prefix.properties,0);
    result.extensions = *merge(merge,original.extensions,geometry.extensions,ordinary.extensions,0);
    for (const auto& row : callout_suffix) result.properties.at("state").at("overrides").push_back(row);
    validate_annotation_entity(result);
    return result;
}

nlohmann::json encode_joint_translation_intent(const JointTranslationIntent& intent) {
    return joint_translation_to_json(intent);
}

JointTranslationIntent decode_joint_translation_intent(const nlohmann::json& value) {
    return joint_translation_from_json(value);
}

static void validate_active_joint_targets(const std::map<std::string, Entity, std::less<>>& source,
    const JointTranslationIntent& intent, const ConstraintPhaseScope& scope) {
    (void)resolve_joint_translation_offsets(source,intent);
    const auto require_active=[&](const auto& ids) {
        for (const auto& id : ids) if (scope.inactive_owner_ids.contains(id))
            throw std::invalid_argument("Joint edit target belongs to an inactive design: " + id);
    };
    require_active(intent.rigid_boundary_ids);
    require_active(intent.rigid_stroke_ids);
    require_active(intent.partial_wall_ids);
    require_active(intent.dimension_ids);
}

static void validate_inactive_joint_records(const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& candidate, const ConstraintPhaseScope& scope) {
    for (const auto& registry : scope.registries)
        if (!candidate.contains(registry.registry_id) ||
            !exact_entity_payload(source.at(registry.registry_id),candidate.at(registry.registry_id)))
            throw std::invalid_argument("Joint constraint edits cannot change saved design choices");
    for (const auto& id : scope.inactive_owner_ids)
        if (!candidate.contains(id) || !exact_entity_payload(source.at(id),candidate.at(id)))
            throw std::invalid_argument("Joint constraint edits cannot change an inactive design object: " + id);
}

static std::map<std::string, Entity, std::less<>> joint_rigid_replay_source_impl(
    const std::map<std::string, Entity, std::less<>>& source, const JointTranslationIntent& intent,
    const ConstraintPhaseScope* scope) {
    if (scope) validate_active_joint_targets(source,intent,*scope);
    const auto resolved = resolve_joint_translation_offsets(source, intent);
    if (!intent.per_owner_rigid_completion && intent.owner_transformations.empty()) return source;
    std::vector<BoundaryTransformation> boundaries;
    for (const auto& id : intent.rigid_boundary_ids) {
        const auto& entity = source.at(id);
        if (entity.properties.contains("wall_measurement_source") ||
            entity.extensions.contains("measurement_linework_sources") || entity.extensions.contains("measurement_linework_group"))
            continue;
        boundaries.push_back({id, resolved.owner_transforms.at(id)});
    }
    auto result=boundaries.empty() ? source : transformed_boundary_entities_per_owner_batch(source, boundaries);
    if (scope) validate_inactive_joint_records(source,result,*scope);
    return result;
}

std::map<std::string, Entity, std::less<>> joint_rigid_replay_source(
    const std::map<std::string, Entity, std::less<>>& source, const JointTranslationIntent& intent) {
    return joint_rigid_replay_source_impl(source,intent,nullptr);
}

std::map<std::string, Entity, std::less<>> joint_rigid_replay_source_active_phase(
    const std::map<std::string, Entity, std::less<>>& source, const JointTranslationIntent& intent) {
    const auto scope=constraint_phase_scope(source);
    return joint_rigid_replay_source_impl(source,intent,&scope);
}

static void complete_joint_rigid_sources_impl(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool physical_sources_ready, const ConstraintPhaseScope* scope) {
    if (scope) {
        validate_active_joint_targets(source,intent,*scope);
        validate_inactive_joint_records(source,candidate,*scope);
    }
    if (!intent.per_owner_rigid_completion && intent.owner_transformations.empty())
        throw std::invalid_argument("Rigid source completion requires a captured v4 owner intent");
    const auto resolved = resolve_joint_translation_offsets(source,intent);
    std::set<std::string,std::less<>> semantic_available;
    for (const auto& [id,entity] : candidate) { (void)entity; semantic_available.insert(id); }
    for (const auto& [id,entity] : candidate) {
        (void)id;
        if (entity.type != "model_phases") continue;
        const auto phases = ModelPhases::from_json(entity.properties.at("model"));
        const auto active = phases.active_state();
        for (const auto& member : phases.entity_ids())
            if (!active.contains(member) || active.at(member) == ModelPhase::demolished) semantic_available.erase(member);
    }
    const auto checks = measurement_linework_source_checks(candidate,&semantic_available);
    for (const auto& id : intent.rigid_boundary_ids) {
        const auto& original = source.at(id);
        const bool walls = original.properties.contains("wall_measurement_source");
        const bool strokes = original.extensions.contains("measurement_linework_sources") ||
            original.extensions.contains("measurement_linework_group");
        if (!walls && !strokes) continue;
        if (walls && strokes) throw std::invalid_argument("Rigid joint source owner has conflicting producer dialects");
        if (walls && !physical_sources_ready) continue;
        auto boundary = decode_identified_boundary_entity(original);
        const auto& transform = resolved.owner_transforms.at(id);
        Boundary expected;
        for (const auto& edge : boundary.segments) expected.push_back(transform_segment(edge.segment,transform));
        RigidExteriorAlignment correspondence;
        Boundary aligned;
        nlohmann::json lineage;
        if (walls) {
            if (identity_rigid_transform(transform)) {
                // A mixed group may explicitly retain an identity consumer.
                // Keep its exact existing proof (including sequential v2
                // lineage); no producer normalization owns a geometry edit.
                if (decode_identified_boundary_entity(candidate.at(id)) != boundary ||
                    candidate.at(id).properties.at("wall_measurement_source") != original.properties.at("wall_measurement_source") ||
                    !wall_measurement_source_current(candidate,candidate.at(id)))
                    throw std::invalid_argument("Rigid joint identity source owner changed its retained geometry or lineage");
                continue;
            }
            const auto ids = exterior_wall_measurement_source_ids(original);
            const auto derived = derive_replacement_exterior_wall_measurement(candidate,original,ids);
            aligned = uniquely_aligned_rigid_exterior(expected,derived.boundary,transform,&correspondence);
            // The typed physical redraw owns its lineage and child identity.
            // Recheck it against independent final walls without inventing a
            // second numerical source dialect or a raw lineage authority.
            if (candidate.at(id).properties.at("wall_measurement_source") != derived.source)
                throw std::invalid_argument("Rigid joint physical redraw differs from exact final source lineage");
        } else {
            const auto check = checks.find(id);
            if (check == checks.end() || !check->second.proposed_boundary || !check->second.proposed_lineage.is_array())
                throw std::invalid_argument("Rigid joint measured redraw has no unique final source face: " +
                    (check == checks.end() ? std::string("missing lineage") : check->second.diagnostic));
            aligned = uniquely_aligned_rigid_exterior(expected,*check->second.proposed_boundary,transform,&correspondence);
            const auto& proposed = check->second.proposed_lineage;
            if (proposed.size() != aligned.size()) throw std::invalid_argument("Rigid joint measured redraw lost source edge lineage");
            lineage = nlohmann::json::array();
            for (std::size_t index=0; index<aligned.size(); ++index) {
                const auto mapped = (correspondence.offset + (correspondence.reversed ? aligned.size()-index : index)) % aligned.size();
                auto uses = proposed.at(mapped);
                if (correspondence.reversed) for (auto& use : uses)
                    use.at("reversed") = !use.at("reversed").get<bool>();
                lineage.push_back(std::move(uses));
            }
        }
        for (std::size_t index=0; index<aligned.size(); ++index) boundary.segments[index].segment = aligned[index];
        const auto current = decode_identified_boundary_entity(candidate.at(id));
        if (boundary != current) {
            BoundaryGeometryEdit correction;
            correction.kind = BoundaryGeometryEditKind::redefine_boundary;
            correction.boundary_id = correction.target_id = id;
            correction.replacement_segments = encode_identified_boundary_entity(boundary).properties.at("segments");
            // Preserve receipt/derivation evidence by appending the precise
            // independently derived correction, rather than changing its bytes.
            candidate = edited_boundary_entities(candidate,correction);
        }
        if (strokes) {
            auto& owner = candidate.at(id);
            owner.extensions["measurement_linework_sources"] = std::move(lineage);
            if (original.extensions.contains("measurement_linework_group")) {
                const auto& group = checks.at(id).proposed_group;
                if (!group.is_object()) throw std::invalid_argument("Rigid joint measured group lacks complete final membership");
                owner.extensions["measurement_linework_group"] = group;
            }
        }
    }
    if (!physical_sources_ready) return;
    const auto verified = measurement_linework_source_checks(candidate,&semantic_available);
    const auto previous = measurement_linework_source_checks(source,&semantic_available);
    for (const auto& [id,old] : previous)
        if ((!scope || !scope->inactive_owner_ids.contains(id)) && old.current &&
            !measurement_linework_source_current(verified,candidate.at(id)))
            throw std::invalid_argument("Rigid joint redraw would stale a previously current measured consumer: " + id);
    for (const auto& id : intent.rigid_boundary_ids)
        if (source.at(id).properties.contains("wall_measurement_source") && !wall_measurement_source_current(candidate,candidate.at(id)))
            throw std::invalid_argument("Rigid joint physical redraw did not retain exact current source geometry");
    if (scope) validate_inactive_joint_records(source,candidate,*scope);
}

void complete_joint_rigid_sources(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool physical_sources_ready) {
    complete_joint_rigid_sources_impl(source,candidate,intent,physical_sources_ready,nullptr);
}

void complete_joint_rigid_sources_active_phase(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool physical_sources_ready) {
    const auto scope=constraint_phase_scope(source);
    complete_joint_rigid_sources_impl(source,candidate,intent,physical_sources_ready,&scope);
}

static void validate_joint_rigid_topology_impl(const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    const ConstraintPhaseScope* scope) {
    if (scope) {
        validate_active_joint_targets(source,intent,*scope);
        validate_inactive_joint_records(source,candidate,*scope);
    }
    if (!intent.per_owner_rigid_completion && intent.owner_transformations.empty())
        throw std::invalid_argument("Rigid topology admission requires a captured v4 owner intent");
    const auto resolved = resolve_joint_translation_offsets(source,intent);
    std::set<std::string,std::less<>> semantic_available;
    for (const auto& [id,entity] : candidate) { (void)entity; semantic_available.insert(id); }
    for (const auto& [id,entity] : candidate) {
        (void)id;
        if (entity.type != "model_phases") continue;
        const auto phases = ModelPhases::from_json(entity.properties.at("model"));
        const auto active = phases.active_state();
        for (const auto& member : phases.entity_ids())
            if (!active.contains(member) || active.at(member) == ModelPhase::demolished) semantic_available.erase(member);
    }
    const auto measured_checks = measurement_linework_source_checks(candidate,&semantic_available);
    auto topology_source = source;
    for (const auto& id : intent.rigid_boundary_ids) {
        auto expected = decode_identified_boundary_entity(source.at(id));
        const auto after = decode_identified_boundary_entity(candidate.at(id));
        if (expected.segments.size() != after.segments.size())
            throw std::invalid_argument("Rigid joint redraw changed selected boundary topology");
        const auto& transform = resolved.owner_transforms.at(id);
        const bool source_bound = source.at(id).properties.contains("wall_measurement_source") ||
            source.at(id).extensions.contains("measurement_linework_sources") || source.at(id).extensions.contains("measurement_linework_group");
        Boundary expected_geometry;
        for (const auto& edge : expected.segments) expected_geometry.push_back(transform_segment(edge.segment,transform));
        if (source_bound) {
            if (source.at(id).properties.contains("wall_measurement_source")) {
                if (!identity_rigid_transform(transform)) {
                    const auto derived = derive_replacement_exterior_wall_measurement(candidate,source.at(id),
                        exterior_wall_measurement_source_ids(source.at(id)));
                    expected_geometry = uniquely_aligned_rigid_exterior(expected_geometry,derived.boundary,transform);
                } else if (candidate.at(id).properties.at("wall_measurement_source") != source.at(id).properties.at("wall_measurement_source"))
                    throw std::invalid_argument("Rigid joint identity source owner changed its retained lineage");
            } else {
                const auto check = measured_checks.find(id);
                if (check == measured_checks.end() || !check->second.proposed_boundary)
                    throw std::invalid_argument("Rigid joint measured topology has no current source correspondence");
                expected_geometry = uniquely_aligned_rigid_exterior(expected_geometry,*check->second.proposed_boundary,transform);
            }
        }
        for (std::size_t index=0; index<expected.segments.size(); ++index) {
            auto& edge = expected.segments[index];
            const auto& actual = after.segments[index];
            edge.segment = expected_geometry[index];
            if (edge.segment_id != actual.segment_id || edge.start_vertex_id != actual.start_vertex_id ||
                edge.end_vertex_id != actual.end_vertex_id || edge.segment.start.x != actual.segment.start.x ||
                edge.segment.start.y != actual.segment.start.y || edge.segment.end.x != actual.segment.end.x ||
                edge.segment.end.y != actual.segment.end.y || edge.segment.sweep_radians != actual.segment.sweep_radians)
                throw std::invalid_argument("Rigid joint source redraw cannot preserve exact selected boundary targets");
        }
        // The source operator explicitly owns reflected winding for this exact
        // boundary. All unselected analytical winding and physical contacts keep
        // the ordinary topology admission rules.
        topology_source.at(id).properties["segments"] = encode_identified_boundary_entity(expected).properties.at("segments");
        if (source.at(id).properties.contains("wall_measurement_source") && !wall_measurement_source_current(candidate,candidate.at(id)))
            throw std::invalid_argument("Rigid joint source redraw left a selected physical boundary stale");
    }
    for (const auto& id : intent.rigid_boundary_ids)
        if ((source.at(id).extensions.contains("measurement_linework_sources") || source.at(id).extensions.contains("measurement_linework_group")) &&
            !measurement_linework_source_current(measured_checks,candidate.at(id)))
            throw std::invalid_argument("Rigid joint source redraw left a selected measured boundary stale");
    for (const auto& [id,entity] : source) {
        (void)id;
        if (entity.type != "constraint") continue;
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.supported() || decoded.constraint->relation != ConstraintRelationKind::fixed_anchor) continue;
        if (scope && !constraint_participates(*decoded.constraint,*scope)) continue;
        const auto& binding = decoded.constraint->bindings.front();
        Vec2 actual;
        if (binding.segment_id.empty()) {
            const auto& baseline = candidate.at(binding.owner_id).properties.at("baseline");
            const auto& value = baseline.at(binding.role == WallEndpointRole::start ? "start" : "end");
            actual = {value.at(0).get<double>(),value.at(1).get<double>()};
        } else {
            const auto owner = resolve_constraint_segment_owner(candidate.at(binding.owner_id));
            const auto edge = std::find_if(owner.segments.begin(),owner.segments.end(),[&](const auto& segment) {
                return segment.segment_id == binding.segment_id &&
                    (binding.role == WallEndpointRole::start ? segment.start_vertex_id : segment.end_vertex_id) == binding.vertex_id;
            });
            if (edge == owner.segments.end()) throw std::invalid_argument("Rigid joint source redraw lost a fixed anchor binding");
            actual = binding.role == WallEndpointRole::start ? edge->segment.start : edge->segment.end;
        }
        if (actual.x != decoded.constraint->anchor->x || actual.y != decoded.constraint->anchor->y)
            throw std::invalid_argument("Rigid joint final source redraw conflicts with an unchanged exact fixed anchor");
    }
    std::map<std::string,PlanarTransform,std::less<>> wall_transforms;
    for (const auto& id : intent.partial_wall_ids) wall_transforms.emplace(id,resolved.owner_transforms.at(id));
    const std::set<std::string,std::less<>> rigid_walls(intent.partial_wall_ids.begin(),intent.partial_wall_ids.end());
    if (scope) validate_active_phase_constraint_edit_topology(topology_source,candidate,rigid_walls,wall_transforms);
    else validate_constraint_edit_topology(topology_source,candidate,rigid_walls,wall_transforms);
}

void validate_joint_rigid_topology(const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent) {
    validate_joint_rigid_topology_impl(source,candidate,intent,nullptr);
}

void validate_joint_rigid_topology_active_phase(const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent) {
    const auto scope=constraint_phase_scope(source);
    validate_joint_rigid_topology_impl(source,candidate,intent,&scope);
}

static void complete_joint_rigid_consequences_impl(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool complete_area_callouts, const ConstraintPhaseScope* scope) {
    if (scope) {
        validate_active_joint_targets(source,intent,*scope);
        validate_inactive_joint_records(source,candidate,*scope);
    }
    if (!intent.per_owner_rigid_completion && intent.owner_transformations.empty()) return;
    const auto resolved = resolve_joint_translation_offsets(source, intent);
    for (const auto& [id, original] : source) {
        if (scope && scope->inactive_owner_ids.contains(id)) continue;
        if (original.type == "constraint") {
            const auto decoded = decode_constraint_entity(original);
            if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
            if (scope && !constraint_participates(*decoded.constraint,*scope)) continue;
            auto expected = *decoded.constraint;
            if (expected.relation != ConstraintRelationKind::horizontal && expected.relation != ConstraintRelationKind::vertical)
                continue; // Fixed anchors retain their saved world coordinates.
            const auto& bindings = expected.bindings;
            if (bindings.empty() || !std::all_of(bindings.begin(), bindings.end(), [&](const auto& binding) {
                return resolved.owner_transforms.contains(binding.owner_id);
            })) continue; // A relation crossing the selected set stays a hard solve constraint.
            const auto& transform = resolved.owner_transforms.at(bindings.front().owner_id);
            if (!std::all_of(bindings.begin(), bindings.end(), [&](const auto& binding) {
                return resolved.owner_transforms.at(binding.owner_id) == transform;
            })) continue;
            if (std::abs(std::remainder(transform.rotation_radians, std::numbers::pi / 2)) > 1e-12)
                throw std::invalid_argument("Axis-locked selected geometry requires a quarter-turn rigid rotation");
            if (std::llround(transform.rotation_radians / (std::numbers::pi / 2)) % 2 != 0)
                expected.relation = expected.relation == ConstraintRelationKind::horizontal ?
                    ConstraintRelationKind::vertical : ConstraintRelationKind::horizontal;
            const auto replacement = encode_constraint_entity(expected, &original);
            if (candidate.at(id) != original && candidate.at(id) != replacement)
                throw std::invalid_argument("Rigid joint completion overlaps a persisted relation edit");
            candidate.at(id) = replacement;
        } else if (original.type == "opening") {
            // A corner owner's two cuts are derived together from the final
            // pair of hosts. Single-wall replay must not overwrite that cohort.
            if (original.properties.contains("corner_window_id")) continue;
            std::string host, diagnostic;
            if (!read_document_wall_id(original, host, diagnostic)) throw std::invalid_argument(diagnostic);
            if (scope && scope->inactive_owner_ids.contains(host)) continue;
            const auto found = resolved.owner_transforms.find(host);
            if (found == resolved.owner_transforms.end()) continue;
            const auto replacement = replay_rigid_source_opening(original, found->second);
            if (candidate.at(id) != original && candidate.at(id) != replacement)
                throw std::invalid_argument("Rigid joint completion overlaps a hosted opening edit");
            candidate.at(id) = replacement;
        } else if (original.type == kAnnotationEntityType) {
            // Wall label offsets have geometry-relative semantics. Area
            // callouts use their final analytical anchors below, after redraw.
            // Child placements remain the explicit presentation translation lane.
            auto& entity = candidate.at(id);
            validate_annotation_entity(original);
            auto& rows = entity.properties.at("state").at("overrides");
            for (const auto& record : original.properties.at("state").at("overrides")) {
                const auto& kind = record.at("target_kind");
                if (kind != "wall_dimension") continue;
                if (scope && scope->inactive_owner_ids.contains(record.at("target_id").get<std::string>())) continue;
                const auto found = resolved.owner_transforms.find(record.at("target_id").get<std::string>());
                if (found == resolved.owner_transforms.end()) continue;
                auto expected = record;
                const auto& transform = found->second;
                if (transform.rotation_radians == 0.0 && !transform.flip_horizontal && !transform.flip_vertical)
                    continue; // Translation does not change a relative label basis.
                const PlanarTransform basis{{}, transform.rotation_radians, transform.flip_horizontal, transform.flip_vertical, {}};
                if (record.contains("plan_label_offset_m")) {
                    const auto& offset = record.at("plan_label_offset_m");
                    const auto next = transform_point({offset.at(0).get<double>(), offset.at(1).get<double>()}, basis);
                    if (!std::isfinite(next.x) || !std::isfinite(next.y)) throw std::invalid_argument("Rigid joint label offset overflows");
                    expected["plan_label_offset_m"] = nlohmann::json::array({next.x, next.y});
                }
                if (record.contains("plan_label_rotation_radians")) {
                    const auto angle = record.at("plan_label_rotation_radians").get<double>();
                    const auto direction = transform_point({std::cos(angle), std::sin(angle)}, basis);
                    expected["plan_label_rotation_radians"] = std::atan2(direction.y, direction.x);
                }
                const auto target = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                    return row.at("target_kind") == kind && row.at("target_id") == record.at("target_id");
                });
                if (target == rows.end() || (*target != record && *target != expected))
                    throw std::invalid_argument("Rigid joint completion overlaps an owned label edit");
                *target = std::move(expected);
            }
            validate_annotation_entity(entity);
        }
    }
    if (complete_area_callouts) {
        std::map<std::string,PlanarTransform,std::less<>> transforms;
        for (const auto& id : intent.rigid_boundary_ids) transforms.emplace(id,resolved.owner_transforms.at(id));
        for (const auto& change : transformed_area_callout_entities(source,candidate,transforms))
            candidate.at(change.entity.id) = merge_selection_annotation_entities(source.at(change.entity.id),
                change.entity,candidate.at(change.entity.id),AnnotationMergeMode::source_callout_proof);
    }
    if (scope) validate_inactive_joint_records(source,candidate,*scope);
}

void complete_joint_rigid_consequences(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool complete_area_callouts) {
    complete_joint_rigid_consequences_impl(source,candidate,intent,complete_area_callouts,nullptr);
}

void complete_joint_rigid_consequences_active_phase(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool complete_area_callouts) {
    const auto scope=constraint_phase_scope(source);
    complete_joint_rigid_consequences_impl(source,candidate,intent,complete_area_callouts,&scope);
}

nlohmann::json phase_entity_import_proof_to_json(const PhaseEntityImportProof& proof) {
    if (proof.message.size()>1024 || !is_valid_utf8_without_nul(proof.message))
        document_error(DocumentErrorCode::invalid_entity,"Phase import proof message is invalid");
    const auto validate_ids=[](const std::vector<std::string>& ids,bool required) {
        std::set<std::string,std::less<>> unique;
        if (required && ids.empty())
            document_error(DocumentErrorCode::invalid_entity,"Phase import proof requires a nonempty identity inventory");
        for (const auto& id:ids)
            if (!is_valid_identifier(id) || !unique.insert(id).second)
                document_error(DocumentErrorCode::invalid_entity,"Phase import proof identity inventory is invalid");
    };
    validate_ids(proof.registry_ids,true);validate_ids(proof.entity_ids,true);validate_ids(proof.asset_ids,false);
    validate_ids(proof.reviewed_existing_hierarchy_ids,false);
    nlohmann::json result{{"version",1},{"kind","import_phase_entities"},
        {"expected_revision",proof.expected_revision},{"message",proof.message},
        {"registry_ids",proof.registry_ids},{"entity_ids",proof.entity_ids},{"asset_ids",proof.asset_ids},
        {"reviewed_existing_hierarchy_ids",proof.reviewed_existing_hierarchy_ids}};
    validate_json_object(result,DocumentErrorCode::invalid_entity,"Phase import proof");
    return result;
}

PhaseEntityImportProof phase_entity_import_proof_from_json(const nlohmann::json& value) {
    command_exact_fields(value,{"version","kind","expected_revision","message","registry_ids","entity_ids","asset_ids","reviewed_existing_hierarchy_ids"},
        DocumentErrorCode::invalid_entity,"Phase import proof");
    if (!value.at("version").is_number_integer() || value.at("version")!=1 ||
        value.at("kind")!="import_phase_entities" || !value.at("message").is_string())
        document_error(DocumentErrorCode::invalid_entity,"Phase import proof envelope is invalid");
    PhaseEntityImportProof result;
    result.expected_revision=command_revision(value.at("expected_revision"),"Phase import proof revision");
    result.message=value.at("message").get<std::string>();
    const auto ids=[&](std::string_view field) {
        const auto& records=value.at(std::string(field));
        if (!records.is_array()) document_error(DocumentErrorCode::invalid_entity,"Phase import inventory must be an array");
        std::vector<std::string> output;
        for (const auto& item:records) output.push_back(command_string(item,field,kMaximumIdBytes));
        return output;
    };
    result.registry_ids=ids("registry_ids");result.entity_ids=ids("entity_ids");result.asset_ids=ids("asset_ids");
    result.reviewed_existing_hierarchy_ids=ids("reviewed_existing_hierarchy_ids");
    (void)phase_entity_import_proof_to_json(result);
    return result;
}

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
        } else if constexpr (std::is_same_v<T, ImportPhaseEntities>) {
            PhaseEntityImportProof proof{typed.expected_revision,typed.message,typed.registry_ids,{},{},typed.reviewed_existing_hierarchy_ids};
            for (const auto& change:typed.entity_changes) {
                if (change.kind!=EntityChangeKind::upsert)
                    document_error(DocumentErrorCode::invalid_entity,"Phase import cannot erase entities");
                proof.entity_ids.push_back(change.entity.id);
            }
            for (const auto& change:typed.asset_changes) {
                if (change.kind!=AssetChangeKind::upsert)
                    document_error(DocumentErrorCode::invalid_asset,"Phase import cannot erase assets");
                proof.asset_ids.push_back(change.asset.id);
            }
            (void)phase_entity_import_proof_to_json(proof);
            auto encoded=command_to_json(ApplyEntityChanges{typed.expected_revision,typed.entity_changes,{},typed.message});
            encoded["kind"]="import_phase_entities";
            encoded["registry_ids"]=typed.registry_ids;
            encoded["reviewed_existing_hierarchy_ids"]=typed.reviewed_existing_hierarchy_ids;
            encoded["asset_changes"]=command_asset_references_to_json(typed.asset_changes);
            return encoded;
        } else if constexpr (std::is_same_v<T, TransformBoundaries>) {
            auto encoded = command_to_json(ApplyEntityChanges{
                typed.expected_revision, typed.entity_changes, {}, typed.message});
            encoded["kind"] = "transform_boundaries";
            encoded.erase("asset_changes");
            const bool per_owner = per_owner_rigid_transform(typed);
            if (per_owner || typed.wall_dimension_completion || typed.measured_stroke_transform_completion) {
                encoded["version"] = per_owner ? 3 : 2;
                encoded["wall_dimension_completion"] = typed.wall_dimension_completion;
                encoded["measured_stroke_transform_completion"] = typed.measured_stroke_transform_completion;
            }
            encoded["transformations"] = nlohmann::json::array();
            if (!per_owner && typed.transformations.empty())
                document_error(DocumentErrorCode::invalid_entity, "Boundary transform group is empty");
            std::set<std::string> owners;
            const PlanarTransform shared = typed.transformations.empty() ? PlanarTransform{} : typed.transformations.front().transform;
            for (const auto& transformation : typed.transformations) {
                if (per_owner) validate_rigid_owner_transform({transformation.boundary_id, transformation.transform});
                if (!per_owner && !(transformation.transform == shared))
                    document_error(DocumentErrorCode::invalid_entity, "Boundary transform group requires one shared transform");
                if (!owners.insert(transformation.boundary_id).second)
                    document_error(DocumentErrorCode::duplicate_change, "Boundary is transformed more than once");
                encoded["transformations"].push_back(command_to_json(
                    TransformBoundary{typed.expected_revision, transformation}).at("transformation"));
            }
            if (per_owner) {
                encoded["per_owner_transform_completion"] = true;
                encoded["source_transformations"] = nlohmann::json::array();
                std::set<std::string> source_owners, witnesses;
                for (const auto& intent : typed.source_transformations) {
                    validate_rigid_owner_transform(intent);
                    if (owners.contains(intent.owner_id) || !source_owners.insert(intent.owner_id).second)
                        throw std::invalid_argument("Rigid source transforms require unique disjoint owners");
                    encoded["source_transformations"].push_back({{"owner_id", intent.owner_id},
                        {"transform", command_transform_to_json(intent.transform)}});
                }
                for (const auto& change : typed.entity_changes) {
                    if (change.kind != EntityChangeKind::upsert ||
                        (change.entity.type != "wall" && change.entity.type != "measurement_linework")) continue;
                    if (!source_owners.contains(change.entity.id) || !witnesses.insert(change.entity.id).second)
                        throw std::invalid_argument("Rigid source geometry requires exactly one captured-owner intent");
                }
                if (witnesses != source_owners)
                    throw std::invalid_argument("Rigid source transforms must cover exactly their wall/stroke witnesses");
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
            if (has_independent_drawing_removal(typed)) {
                try {
                    validate_independent_drawing_removal_mode(typed);
                    const auto proof=command_to_json(Command{without_independent_drawing_removal(typed)});
                    const bool phase_authoring=has_phase_constraint_authoring(typed);
                    if (proof.at("kind")!="apply_boundary_constraint_changes" || !proof.at("version").is_number_integer() ||
                        proof.at("version").get<int>()<1 || proof.at("version").get<int>()>40 ||
                        (phase_authoring && proof.at("version")!=34))
                        throw std::invalid_argument("Independent drawing removal requires one preceding unnested complete proof");
                    auto encoded=nlohmann::json{{"version",phase_authoring ? 42 : 41},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"independent_drawing_removal_completion",true},
                        {"independent_drawing_removal_intent",typed.independent_drawing_removal_intent},{"proof",proof}};
                    if (encoded.dump().size()>1024*1024)
                        throw std::invalid_argument("Independent drawing removal exceeds the persisted proof budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (has_phase_room_review_completion(typed)) {
                try {
                    validate_phase_room_review_mode(typed);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                    const auto intent=decode_physical_wall_phase_room_review_intent(typed.phase_room_review_intent);
                    if (intent.expected_revision!=typed.expected_revision ||
                        encode_physical_wall_phase_room_review_intent(intent).dump()!=typed.phase_room_review_intent.dump())
                        throw std::invalid_argument("Phase room review must retain its canonical source intent");
                    if (typed.message.size()>1024 || !is_valid_utf8_without_nul(typed.message))
                        throw std::invalid_argument("Serialized phase room review message is invalid");
                    auto encoded=nlohmann::json{{"version",33},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"phase_room_review_completion",true},{"phase_room_review_intent",typed.phase_room_review_intent}};
                    if (encoded.dump().size()>1024*1024)
                        throw std::invalid_argument("Phase room review exceeds the persisted proof budget");
                    return encoded;
#else
                    throw std::invalid_argument("Phase room review requires the production physical-wall engine");
#endif
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (has_room_review_batch_completion(typed)) {
                try { validate_room_review_mode(typed,false); }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (has_selection_completion(typed)) {
                try {
                    if (!typed.selection_completion || typed.selection_entity_changes.size() > 1000)
                        throw std::invalid_argument("Selection completion requires its marker and bounded ordinary lane");
                    std::unordered_set<std::string> targets;
                    for (const auto& change : typed.selection_entity_changes)
                        if (change.kind != EntityChangeKind::upsert || !targets.insert(change.entity.id).second ||
                            !supported_selection_entity(change.entity))
                            throw std::invalid_argument("Selection completion requires unique supported nonwall upserts");
                    const auto proof = command_to_json(Command{without_selection_completion(typed)});
                    const auto version = proof.at("version").get<int>();
                    if (version < 1 || (version > 21 && version != 23 && version != 34))
                        throw std::invalid_argument("Selection completion requires one preceding typed proof");
                    auto encoded = nlohmann::json{{"version",22},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"selection_completion",true},{"proof",proof},
                        {"selection_entity_changes",command_to_json(ApplyEntityChanges{
                            typed.expected_revision,typed.selection_entity_changes,{},typed.message}).at("entity_changes")}};
                    if (encoded.dump().size() > 1024 * 1024)
                        throw std::invalid_argument("Selection completion exceeds the persisted proof budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (has_phase_constraint_authoring(typed) && !has_disto_measurement_completion(typed)) {
                try {
                    validate_phase_constraint_authoring_mode(typed);
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                    const auto intent=decode_phase_constraint_authoring_intent(typed.phase_constraint_authoring_intent);
                    if (intent.expected_revision!=typed.expected_revision ||
                        encode_phase_constraint_authoring_intent(intent).dump()!=typed.phase_constraint_authoring_intent.dump())
                        throw std::invalid_argument("Active design authoring must retain its canonical source intent");
                    if (typed.message.size()>1024 || !is_valid_utf8_without_nul(typed.message) ||
                        intent.intent.message.size()>1024 || !is_valid_utf8_without_nul(intent.intent.message))
                        throw std::invalid_argument("Serialized active design authoring message is invalid");
                    auto encoded=nlohmann::json{{"version",34},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"phase_constraint_authoring_completion",true},
                        {"phase_constraint_authoring_intent",typed.phase_constraint_authoring_intent}};
                    if (encoded.dump().size()>1024*1024)
                        throw std::invalid_argument("Active design authoring exceeds the persisted proof budget");
                    return encoded;
#else
                    throw std::invalid_argument("Active design authoring requires the production constraint engine");
#endif
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (typed.wall_dimension_completion) {
                try {
                    validate_wall_dimension_completion(typed);
                    auto original = typed;
                    original.wall_dimension_completion = false;
                    auto proof = command_to_json(Command{original});
                    if (proof.at("version") != 10 && proof.at("version") != 11)
                        throw std::invalid_argument("Wall callout completion requires the existing rigid wall or wall/stroke dialect");
                    auto encoded = nlohmann::json{{"version",21},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"wall_dimension_completion",true},{"proof",std::move(proof)}};
                    if (encoded.dump().size() > 1024 * 1024)
                        throw std::invalid_argument("Wall callout completion exceeds the persisted proof budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (typed.curve_construction_completion) {
                try {
                    validate_curve_construction_completion(typed);
                    auto original = typed;
                    original.curve_construction_completion = false;
                    auto proof = command_to_json(Command{original});
                    if (proof.at("version").get<int>() < 2 || proof.at("version").get<int>() > 11 ||
                        proof.at("version") == 8 || proof.at("version") == 10)
                        throw std::invalid_argument("Curve construction requires its existing endpoint/source proof");
                    auto encoded = nlohmann::json{{"version",23},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"curve_construction_completion",true},{"proof",std::move(proof)}};
                    if (encoded.dump().size() > 1024*1024)
                        throw std::invalid_argument("Curve construction exceeds the persisted proof budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            try { validate_room_aware_wall_split_mode(typed); validate_wall_merge_mode(typed); }
            catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
            if (typed.wall_merge) {
                if (typed.message.size()>1024 || !is_valid_utf8_without_nul(typed.message))
                    document_error(DocumentErrorCode::invalid_entity,"Serialized wall merge message is invalid");
                return nlohmann::json{{"version",20},{"kind","apply_boundary_constraint_changes"},
                    {"expected_revision",typed.expected_revision},{"message",typed.message},
                    {"wall_merge",encode_wall_merge(*typed.wall_merge)}};
            }
            if (has_disto_measurement_completion(typed)) {
                try {
                    if (!typed.disto_measurement_completion || !typed.disto_measurement ||
                        !is_valid_identifier(typed.disto_measurement->owner_id))
                        throw std::invalid_argument("DISTO completion requires one identified observation");
                    const auto& observation = *typed.disto_measurement;
                    auto encoded = nlohmann::json{{"version", 19}, {"kind", "apply_boundary_constraint_changes"},
                        {"expected_revision", typed.expected_revision}, {"message", typed.message},
                        {"disto_measurement_completion", true},
                        {"disto_measurement", {{"owner_id", observation.owner_id},
                            {"record", nlohmann::json::parse(disto_measurement_json(observation.record))},
                            {"replace_existing", observation.replace_existing}}},
                        {"proof", command_to_json(Command{without_disto_measurement(typed)})}};
                    if (encoded.dump().size() > 1024 * 1024)
                        throw std::invalid_argument("DISTO completion exceeds the persisted proof budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
            }
            if (has_room_review_completion(typed)) {
                try {
                    validate_room_review_mode(typed,false);
                    const bool geometry=has_room_review_geometry_completion(typed);
                    const bool batch=has_room_review_batch_completion(typed);
                    const bool context_review=room_review_context_selection(typed.room_review_intent);
                    const int geometry_version=geometry ? room_review_geometry_dialect(typed,room_review_geometry_command(typed))
                        : context_review ? 29 : 18;
                    const int version=batch ? 27 : geometry_version;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                    for (const auto& intent : room_review_intents(typed))
                        if (!intent.is_null()) (void)decode_physical_wall_room_review_intent(intent);
#endif
                    auto encoded=nlohmann::json{{"version",version},{"kind","apply_boundary_constraint_changes"},
                        {"expected_revision",typed.expected_revision},{"message",typed.message},
                        {"room_review_completion",true},{"room_review_intent",typed.room_review_intent}};
                    if (geometry) {
                        encoded["room_review_geometry_completion"]=true;
                        encoded["room_review_geometry_proof"]=typed.room_review_geometry_proof;
                    }
                    if (batch) {
                        encoded["room_review_batch_completion"]=true;
                        encoded["room_review_additional_intents"]=typed.room_review_additional_intents;
                    }
                    if (encoded.dump().size()>1024*1024)
                        throw std::invalid_argument("Room review exceeds the persisted intent budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
            }
            if (has_joint_translation_completion(typed)) {
                try {
                    validate_joint_translation_mode(typed, false);
                    const bool per_target = typed.joint_translation && joint_per_target_presentation(*typed.joint_translation);
                    validate_joint_presentation_proof(typed);
                    auto proof = typed;
                    proof.joint_translation.reset();
                    proof.joint_translation_completion = false;
                    if (per_target) remove_joint_presentation_proof(proof);
                    auto encoded = nlohmann::json{{"version", 17}, {"kind", "apply_boundary_constraint_changes"},
                        {"expected_revision", typed.expected_revision}, {"message", typed.message},
                        {"joint_translation_completion", true},
                        {"joint_translation", typed.joint_translation ? joint_translation_to_json(*typed.joint_translation) : nlohmann::json(nullptr)},
                        {"proof", command_to_json(Command{proof})}};
                    if (per_target)
                        encoded["presentation_proof"] = command_to_json(ApplyEntityChanges{
                            typed.expected_revision, typed.supplemental_entity_changes, {}, typed.message}).at("entity_changes");
                    if (encoded.dump().size() > 1024 * 1024)
                        throw std::invalid_argument("Joint translation exceeds the persisted proof budget");
                    return encoded;
                } catch (const DocumentError&) { throw; }
                catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
            }
            if (std::any_of(typed.boundary_edits.begin(),typed.boundary_edits.end(),[](const auto& edit){return edit.physical_wall_room_repair.has_value();}) ||
                std::any_of(typed.exterior_source_edits.begin(),typed.exterior_source_edits.end(),[](const auto& edit){return edit.physical_wall_room_repair.has_value();}))
                document_error(DocumentErrorCode::invalid_entity,"Physical room repair requires its exclusive same-ID boundary command");
            try { validate_dimension_placement_intent(typed, false); validate_rigid_group_intent(typed, false); }
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
                // Compose only these two typed dialects; select sixteen last
                // so stripped child/placement lanes cannot erase its marker.
                if (has_rigid_group_completion(typed)) {
                    encoded["version"] = 16;
                    encoded["source_completion"] = has_exterior_source_completion(typed);
                    encoded["supplemental_source_completion"] = has_supplemental_source_completion(typed);
                    encoded["supplemental_asset_reference_completion"] = typed.supplemental_asset_reference_completion;
                    encoded["rigid_wall_transform_completion"] = has_rigid_wall_transform(typed);
                    encoded["measured_source_completion"] = has_measured_source_completion(typed);
                    for (const auto* key : {"wall_edits", "measured_stroke_edits", "exterior_source_edits"})
                        if (!encoded.contains(key)) encoded[key] = nlohmann::json::array();
                    encoded["physical_entity_changes"] = command_to_json(ApplyEntityChanges{
                        typed.expected_revision, typed.physical_entity_changes, {}, typed.message}).at("entity_changes");
                    const auto supplements = command_to_json(ApplyEntityChanges{typed.expected_revision, typed.supplemental_entity_changes,
                        typed.supplemental_asset_reference_completion ? std::vector<AssetChange>{} : typed.supplemental_asset_changes, typed.message});
                    encoded["supplemental_entity_changes"] = supplements.at("entity_changes");
                    encoded["supplemental_asset_changes"] = typed.supplemental_asset_reference_completion ?
                        command_asset_references_to_json(typed.supplemental_asset_changes) : supplements.at("asset_changes");
                    encoded["exterior_corner_move"] = nullptr;
                    encoded["dimension_placement_completion"] = has_dimension_placement_completion(typed);
                    if (!encoded.contains("dimension_placement_moves")) encoded["dimension_placement_moves"] = nlohmann::json::array();
                    encoded["rigid_group_completion"] = true;
                    encoded["rigid_group_transform"] = typed.rigid_group_transform ?
                        command_to_json(Command{*typed.rigid_group_transform}) : nlohmann::json(nullptr);
                    if (encoded.dump().size() > 1024*1024)
                        throw std::invalid_argument("Mixed rigid group proof exceeds the persisted proof budget");
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
            (value.at("version")<1 || value.at("version")>42) ||
            !value.at("kind").is_string()) {
            document_error(DocumentErrorCode::invalid_entity, "serialized command envelope is invalid");
        }
        const auto kind = value.at("kind").get<std::string>();
        if (value.at("version") != 1 && kind != "apply_boundary_constraint_changes" &&
            !(kind == "transform_boundaries" && (value.at("version") == 2 || value.at("version") == 3)))
            document_error(DocumentErrorCode::invalid_entity,"Unsupported command envelope version");
        if (kind=="import_phase_entities") {
            command_exact_fields(value,{"version","kind","expected_revision","message","registry_ids","entity_changes","asset_changes","reviewed_existing_hierarchy_ids"},
                DocumentErrorCode::invalid_entity,"serialized phase import");
            if (!value.at("registry_ids").is_array() || !value.at("reviewed_existing_hierarchy_ids").is_array())
                document_error(DocumentErrorCode::invalid_entity,"Phase import registry and reviewed hierarchy inventories must be arrays");
            auto ordinary=value;ordinary["kind"]="apply_entity_changes";ordinary.erase("registry_ids");
            ordinary.erase("reviewed_existing_hierarchy_ids");
            ordinary["asset_changes"]=nlohmann::json::array();
            const auto decoded=std::get<ApplyEntityChanges>(command_from_json(ordinary));
            ImportPhaseEntities result{decoded.expected_revision,decoded.entity_changes,
                command_asset_references_from_json(value.at("asset_changes"),asset_resolver),decoded.message,{},{}};
            for (const auto& id:value.at("registry_ids"))
                result.registry_ids.push_back(command_string(id,"Phase import registry",kMaximumIdBytes));
            for (const auto& id:value.at("reviewed_existing_hierarchy_ids"))
                result.reviewed_existing_hierarchy_ids.push_back(command_string(id,"Phase import reviewed hierarchy",kMaximumIdBytes));
            (void)command_to_json(result);
            return result;
        }
        if (kind == "transform_boundaries") {
            const bool per_owner = value.at("version") == 3;
            const bool qualified_group = per_owner || value.at("version") == 2;
            if (per_owner) {
                command_exact_fields(value, {"version", "kind", "expected_revision", "message", "entity_changes", "transformations",
                    "wall_dimension_completion", "measured_stroke_transform_completion", "per_owner_transform_completion", "source_transformations"},
                    DocumentErrorCode::invalid_entity, "serialized per-owner rigid transform group");
                if (!value.at("per_owner_transform_completion").is_boolean() ||
                    !value.at("per_owner_transform_completion").get<bool>() ||
                    !value.at("wall_dimension_completion").is_boolean() ||
                    !value.at("measured_stroke_transform_completion").is_boolean() ||
                    !value.at("source_transformations").is_array())
                    throw std::invalid_argument("Per-owner rigid transform requires its explicit typed envelope");
            } else if (qualified_group) {
                command_exact_fields(value, {"version", "kind", "expected_revision", "message", "entity_changes", "transformations", "wall_dimension_completion", "measured_stroke_transform_completion"},
                                     DocumentErrorCode::invalid_entity, "serialized transform group with wall callouts");
                if (!value.at("wall_dimension_completion").is_boolean() ||
                    !value.at("measured_stroke_transform_completion").is_boolean() ||
                    (!value.at("wall_dimension_completion").get<bool>() &&
                     !value.at("measured_stroke_transform_completion").get<bool>()))
                    throw std::invalid_argument("Transform group geometry completion must be explicit");
            } else command_exact_fields(value, {"version", "kind", "expected_revision", "message", "entity_changes", "transformations"},
                                 DocumentErrorCode::invalid_entity, "serialized transform group");
            if (value.dump().size() > 1024 * 1024)
                throw std::invalid_argument("Boundary transform group exceeds the persisted proof budget");
            if (!value.at("transformations").is_array() || (!per_owner && value.at("transformations").empty()))
                document_error(DocumentErrorCode::invalid_entity, "Transformations must be a nonempty array");
            auto ordinary = value;
            ordinary["version"] = 1;
            ordinary["kind"] = "apply_entity_changes";
            ordinary.erase("transformations");
            ordinary.erase("wall_dimension_completion");
            ordinary.erase("measured_stroke_transform_completion");
            ordinary.erase("per_owner_transform_completion");
            ordinary.erase("source_transformations");
            ordinary["asset_changes"] = nlohmann::json::array();
            const auto changes = std::get<ApplyEntityChanges>(command_from_json(ordinary));
            TransformBoundaries result{changes.expected_revision, {}, changes.entity_changes, changes.message};
            result.wall_dimension_completion = qualified_group && value.at("wall_dimension_completion").get<bool>();
            result.measured_stroke_transform_completion = qualified_group && value.at("measured_stroke_transform_completion").get<bool>();
            result.per_owner_transform_completion = per_owner;
            std::set<std::string> owners;
            for (const auto& transformation : value.at("transformations")) {
                const auto single = std::get<TransformBoundary>(command_from_json(nlohmann::json{
                    {"version", 1}, {"kind", "transform_boundary"},
                    {"expected_revision", result.expected_revision}, {"transformation", transformation}}));
                if (!owners.insert(single.transformation.boundary_id).second)
                    document_error(DocumentErrorCode::duplicate_change, "Boundary is transformed more than once");
                if (!per_owner && !result.transformations.empty() &&
                    !(result.transformations.front().transform == single.transformation.transform))
                    document_error(DocumentErrorCode::invalid_entity, "Boundary transform group requires one shared transform");
                result.transformations.push_back(single.transformation);
            }
            if (per_owner) {
                for (const auto& intent : value.at("source_transformations")) {
                    command_exact_fields(intent, {"owner_id", "transform"}, DocumentErrorCode::invalid_entity,
                        "serialized rigid source owner transform");
                    if (!intent.at("owner_id").is_string())
                        throw std::invalid_argument("Rigid source owner identity must be a string");
                    RigidOwnerTransformation decoded{intent.at("owner_id").get<std::string>(),
                        command_transform_from_json(intent.at("transform"))};
                    validate_rigid_owner_transform(decoded);
                    result.source_transformations.push_back(std::move(decoded));
                }
                // Reuse encoding's exact source-witness coverage and duplicate
                // checks. The explicit marker survives empty malformed intent
                // groups so they cannot downgrade into a historical dialect.
                (void)command_to_json(Command{result});
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
            if (value.at("version")==41 || value.at("version")==42) {
                const bool phase_enclosure=value.at("version")==42;
                command_exact_fields(value,{"version","kind","expected_revision","message",
                    "independent_drawing_removal_completion","independent_drawing_removal_intent","proof"},
                    DocumentErrorCode::invalid_entity,"serialized independent drawing removal");
                if (value.dump().size()>1024*1024 || !value.at("independent_drawing_removal_completion").is_boolean() ||
                    !value.at("independent_drawing_removal_completion").get<bool>())
                    throw std::invalid_argument("Independent drawing removal mode or proof budget is invalid");
                const auto& proof=value.at("proof");
                if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
                    proof.at("version").get<std::int64_t>()<1 || proof.at("version").get<std::int64_t>()>40 ||
                    !proof.contains("kind") || proof.at("kind")!=kind ||
                    (phase_enclosure && proof.at("version")!=34))
                    throw std::invalid_argument("Independent drawing removal requires one preceding unnested proof");
                const auto decoded=command_from_json(proof,asset_resolver);
                const auto* original=std::get_if<ApplyBoundaryConstraintChanges>(&decoded);
                if (!original || has_independent_drawing_removal(*original) ||
                    (phase_enclosure ? !has_phase_constraint_authoring(*original) : has_phase_constraint_authoring(*original)) ||
                    original->expected_revision!=command_revision(value.at("expected_revision"),"Independent drawing removal revision") ||
                    !value.at("message").is_string() || value.at("message")!=proof.at("message"))
                    throw std::invalid_argument("Independent drawing removal must retain its original command identity");
                auto result=*original;
                result.independent_drawing_removal_completion=true;
                result.independent_drawing_removal_intent=value.at("independent_drawing_removal_intent");
                validate_independent_drawing_removal_mode(result);
                if (command_to_json(Command{result}).dump()!=value.dump())
                    throw std::invalid_argument("Independent drawing removal proof is not canonical");
                return result;
            }
            if (value.at("version")==34) {
                command_exact_fields(value,{"version","kind","expected_revision","message",
                    "phase_constraint_authoring_completion","phase_constraint_authoring_intent"},
                    DocumentErrorCode::invalid_entity,"serialized active design authoring");
                if (value.dump().size()>1024*1024 || !value.at("phase_constraint_authoring_completion").is_boolean() ||
                    !value.at("phase_constraint_authoring_completion").get<bool>() || !value.at("message").is_string())
                    throw std::invalid_argument("Active design authoring mode, message or proof budget is invalid");
                ApplyBoundaryConstraintChanges result;
                result.expected_revision=command_revision(value.at("expected_revision"),"Active design authoring revision");
                result.message=value.at("message").get<std::string>();
                result.phase_constraint_authoring_completion=true;
                result.phase_constraint_authoring_intent=value.at("phase_constraint_authoring_intent");
                if (command_to_json(Command{result}).dump()!=value.dump())
                    throw std::invalid_argument("Active design authoring requires its canonical envelope");
                return result;
            }
            if (value.at("version")==33) {
                command_exact_fields(value,{"version","kind","expected_revision","message",
                    "phase_room_review_completion","phase_room_review_intent"},
                    DocumentErrorCode::invalid_entity,"serialized phase room review");
                if (value.dump().size()>1024*1024 || !value.at("phase_room_review_completion").is_boolean() ||
                    !value.at("phase_room_review_completion").get<bool>() || !value.at("message").is_string())
                    throw std::invalid_argument("Phase room review mode, message or proof budget is invalid");
                ApplyBoundaryConstraintChanges result;
                result.expected_revision=command_revision(value.at("expected_revision"),"Phase room review revision");
                result.message=value.at("message").get<std::string>();
                result.phase_room_review_completion=true;
                result.phase_room_review_intent=value.at("phase_room_review_intent");
                if (command_to_json(Command{result}).dump()!=value.dump())
                    throw std::invalid_argument("Phase room review requires its canonical envelope");
                return result;
            }
            if (value.at("version") == 23) {
                command_exact_fields(value,{"version","kind","expected_revision","message","curve_construction_completion","proof"},
                    DocumentErrorCode::invalid_entity,"serialized curve construction completion");
                if (value.dump().size() > 1024 * 1024 || !value.at("curve_construction_completion").is_boolean() ||
                    !value.at("curve_construction_completion").get<bool>())
                    throw std::invalid_argument("Curve construction mode or proof budget is invalid");
                const auto& proof = value.at("proof");
                if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
                    proof.at("version").get<std::int64_t>() < 2 || proof.at("version").get<std::int64_t>() > 11 ||
                    proof.at("version") == 8 || proof.at("version") == 10 ||
                    !proof.contains("kind") || proof.at("kind") != kind)
                    throw std::invalid_argument("Curve construction requires one unnested endpoint/source proof");
                const auto decoded = command_from_json(proof, asset_resolver);
                const auto* original = std::get_if<ApplyBoundaryConstraintChanges>(&decoded);
                if (!original || original->curve_construction_completion ||
                    original->expected_revision != command_revision(value.at("expected_revision"),"Curve construction revision") ||
                    !value.at("message").is_string() || value.at("message") != proof.at("message"))
                    throw std::invalid_argument("Curve construction must retain its original command identity");
                auto result = *original;
                result.curve_construction_completion = true;
                validate_curve_construction_completion(result);
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 22) {
                command_exact_fields(value,{"version","kind","expected_revision","message","selection_completion","proof","selection_entity_changes"},
                    DocumentErrorCode::invalid_entity,"serialized selection completion");
                if (value.dump().size() > 1024 * 1024 || !value.at("selection_completion").is_boolean() ||
                    !value.at("selection_completion").get<bool>() || !value.at("selection_entity_changes").is_array() ||
                    value.at("selection_entity_changes").size() > 1000)
                    throw std::invalid_argument("Selection completion mode or proof budget is invalid");
                const auto& proof = value.at("proof");
                if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
                    proof.at("version").get<std::int64_t>() < 1 ||
                    (proof.at("version").get<std::int64_t>() > 21 && proof.at("version") != 23 && proof.at("version") != 34) ||
                    !proof.contains("kind") || proof.at("kind") != kind)
                    throw std::invalid_argument("Selection completion requires one preceding unnested typed proof");
                const auto decoded = command_from_json(proof, asset_resolver);
                const auto* original = std::get_if<ApplyBoundaryConstraintChanges>(&decoded);
                if (!original || has_selection_completion(*original) ||
                    original->expected_revision != command_revision(value.at("expected_revision"),"Selection completion revision") ||
                    !value.at("message").is_string() || value.at("message") != proof.at("message"))
                    throw std::invalid_argument("Selection completion must retain its original command identity");
                auto result = *original;
                result.selection_completion = true;
                result.selection_entity_changes = std::get<ApplyEntityChanges>(command_from_json(nlohmann::json{
                    {"version",1},{"kind","apply_entity_changes"},{"expected_revision",result.expected_revision},
                    {"message",result.message},{"entity_changes",value.at("selection_entity_changes")},
                    {"asset_changes",nlohmann::json::array()}})).entity_changes;
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 21) {
                command_exact_fields(value,{"version","kind","expected_revision","message","wall_dimension_completion","proof"},
                    DocumentErrorCode::invalid_entity,"serialized rigid wall callout completion");
                if (value.dump().size() > 1024 * 1024 || !value.at("wall_dimension_completion").is_boolean() ||
                    !value.at("wall_dimension_completion").get<bool>())
                    throw std::invalid_argument("Wall callout completion mode or proof budget is invalid");
                const auto& proof = value.at("proof");
                if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
                    (proof.at("version") != 10 && proof.at("version") != 11))
                    throw std::invalid_argument("Wall callout completion requires one unnested rigid wall or wall/stroke proof");
                const auto decoded = command_from_json(proof, asset_resolver);
                const auto* original = std::get_if<ApplyBoundaryConstraintChanges>(&decoded);
                if (!original || original->wall_dimension_completion ||
                    command_revision(value.at("expected_revision"),"Wall callout completion revision") != original->expected_revision ||
                    !value.at("message").is_string() || value.at("message") != proof.at("message"))
                    throw std::invalid_argument("Wall callout completion does not retain its original command identity");
                auto result = *original;
                result.wall_dimension_completion = true;
                validate_wall_dimension_completion(result);
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 20) {
                command_exact_fields(value,{"version","kind","expected_revision","message","wall_merge"},
                    DocumentErrorCode::invalid_entity,"serialized wall merge command");
                if(!value.at("message").is_string())document_error(DocumentErrorCode::invalid_entity,"Wall merge message must be a string");
                ApplyBoundaryConstraintChanges result;
                result.expected_revision=command_revision(value.at("expected_revision"),"Wall merge revision");
                result.message=value.at("message").get<std::string>();
                result.wall_merge=decode_wall_merge(value.at("wall_merge"));
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 19) {
                command_exact_fields(value, {"version", "kind", "expected_revision", "message",
                    "disto_measurement_completion", "disto_measurement", "proof"},
                    DocumentErrorCode::invalid_entity, "serialized DISTO completion");
                if (value.dump().size() > 1024 * 1024 || !value.at("disto_measurement_completion").is_boolean() ||
                    !value.at("disto_measurement_completion").get<bool>())
                    throw std::invalid_argument("DISTO completion mode or proof budget is invalid");
                const auto& proof = value.at("proof");
                if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
                    proof.at("version").get<std::int64_t>() < 1 ||
                    (proof.at("version").get<std::int64_t>() > 18 && proof.at("version")!=34))
                    throw std::invalid_argument("DISTO completion requires a preceding command dialect");
                const auto decoded = command_from_json(proof, asset_resolver);
                const auto* original = std::get_if<ApplyBoundaryConstraintChanges>(&decoded);
                if (!original || has_disto_measurement_completion(*original) ||
                    command_revision(value.at("expected_revision"), "DISTO completion revision") != original->expected_revision ||
                    !value.at("message").is_string() || value.at("message") != proof.at("message"))
                    throw std::invalid_argument("DISTO completion does not retain its original command identity");
                const auto& attachment = value.at("disto_measurement");
                command_exact_fields(attachment, {"owner_id", "record", "replace_existing"},
                    DocumentErrorCode::invalid_entity, "serialized DISTO observation");
                if (!attachment.at("replace_existing").is_boolean())
                    throw std::invalid_argument("DISTO replacement choice must be boolean");
                auto result = *original;
                result.disto_measurement = DistoMeasurementAttachment{
                    command_string(attachment.at("owner_id"), "DISTO observation owner"),
                    parse_disto_measurement_json(attachment.at("record").dump()),
                    attachment.at("replace_existing").get<bool>()};
                result.disto_measurement_completion = true;
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version")==18 || value.at("version")==24 || value.at("version")==25 || value.at("version")==26 || value.at("version")==27 || value.at("version")==28 || value.at("version")==29 || value.at("version")==30 || value.at("version")==31 || value.at("version")==32 || value.at("version")==35 || value.at("version")==36 || value.at("version")==37 || value.at("version")==38 || value.at("version")==39 || value.at("version")==40) {
                const bool geometry=value.at("version")!=18 && value.at("version")!=29;
                const bool batch=value.at("version")==27;
                if (batch)
                    command_exact_fields(value,{"version","kind","expected_revision","message","room_review_completion","room_review_intent",
                        "room_review_geometry_completion","room_review_geometry_proof","room_review_batch_completion","room_review_additional_intents"},
                        DocumentErrorCode::invalid_entity,"serialized physical-room review batch");
                else if (geometry)
                    command_exact_fields(value,{"version","kind","expected_revision","message","room_review_completion","room_review_intent",
                        "room_review_geometry_completion","room_review_geometry_proof"},
                        DocumentErrorCode::invalid_entity,"serialized wall and physical-room review");
                else command_exact_fields(value,{"version","kind","expected_revision","message","room_review_completion","room_review_intent"},
                    DocumentErrorCode::invalid_entity,"serialized physical-room review");
                if (value.dump().size()>1024*1024 || !value.at("room_review_completion").is_boolean() ||
                    !value.at("room_review_completion").get<bool>())
                    throw std::invalid_argument("Room review mode or intent budget is invalid");
                const auto ordinary=std::get<ApplyEntityChanges>(command_from_json(nlohmann::json{
                    {"version",1},{"kind","apply_entity_changes"},{"expected_revision",value.at("expected_revision")},
                    {"message",value.at("message")},{"entity_changes",nlohmann::json::array()},{"asset_changes",nlohmann::json::array()}}));
                ApplyBoundaryConstraintChanges result;
                result.expected_revision=ordinary.expected_revision;
                result.message=ordinary.message;
                result.room_review_completion=true;
                result.room_review_intent=value.at("room_review_intent");
                if (!geometry) {
                    const bool context_review=room_review_context_selection(result.room_review_intent);
                    if (context_review!=(value.at("version")==29))
                        throw std::invalid_argument("Room-only review dialect does not match its selection basis");
                }
                if (batch) {
                    if (!value.at("room_review_batch_completion").is_boolean() ||
                        !value.at("room_review_batch_completion").get<bool>() ||
                        !value.at("room_review_additional_intents").is_array() ||
                        value.at("room_review_additional_intents").empty() || value.at("room_review_additional_intents").size()>31)
                        throw std::invalid_argument("Room review batch requires its retained mode and bounded additional decisions");
                    result.room_review_batch_completion=true;
                    result.room_review_additional_intents=value.at("room_review_additional_intents").get<std::vector<nlohmann::json>>();
                }
                if (geometry) {
                    if (!value.at("room_review_geometry_completion").is_boolean() ||
                        !value.at("room_review_geometry_completion").get<bool>())
                        throw std::invalid_argument("Wall room review requires its explicit geometry mode");
                    result.room_review_geometry_completion=true;
                    result.room_review_geometry_proof=value.at("room_review_geometry_proof");
                    const auto geometry_version=room_review_geometry_dialect(result,room_review_geometry_command(result));
                    if (!batch && geometry_version!=value.at("version"))
                        throw std::invalid_argument("Wall room review dialect does not match its geometry proof");
                }
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 17) {
                std::optional<JointTranslationIntent> joint_intent;
                if (value.contains("joint_translation") && !value.at("joint_translation").is_null())
                    joint_intent = joint_translation_from_json(value.at("joint_translation"));
                const bool per_target = joint_intent && joint_per_target_presentation(*joint_intent);
                if (per_target)
                    command_exact_fields(value, {"version", "kind", "expected_revision", "message",
                        "joint_translation_completion", "joint_translation", "proof", "presentation_proof"},
                        DocumentErrorCode::invalid_entity, "serialized joint translation");
                else
                    command_exact_fields(value, {"version", "kind", "expected_revision", "message",
                        "joint_translation_completion", "joint_translation", "proof"},
                        DocumentErrorCode::invalid_entity, "serialized joint translation");
                if (value.dump().size() > 1024 * 1024 || !value.at("joint_translation_completion").is_boolean() ||
                    !value.at("joint_translation_completion").get<bool>())
                    throw std::invalid_argument("Joint translation mode or proof budget is invalid");
                const auto& proof = value.at("proof");
                if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
                    proof.at("version").get<int>() < 1 || proof.at("version").get<int>() >= 16 ||
                    !proof.contains("kind") || proof.at("kind") != kind)
                    throw std::invalid_argument("Joint translation requires one ordinary geometry proof");
                ApplyBoundaryConstraintChanges result;
                if (joint_intent && joint_intent->per_owner_rigid_completion && proof.at("version") == 1 &&
                    proof.contains("boundary_edits") && proof.at("boundary_edits").is_array() && proof.at("boundary_edits").empty()) {
                    // v4 ordinary boundary receipts are reconstructed from their
                    // operators before the lower proof. Empty geometry is legal
                    // only inside this source-qualified wrapper, never as v1.
                    command_exact_fields(proof, {"version", "kind", "expected_revision", "message", "entity_changes", "boundary_edits"},
                        DocumentErrorCode::invalid_entity, "rigid joint prepared boundary proof");
                    auto ordinary = proof;
                    ordinary["kind"] = "apply_entity_changes";
                    ordinary.erase("boundary_edits");
                    ordinary["asset_changes"] = nlohmann::json::array();
                    const auto decoded = std::get<ApplyEntityChanges>(command_from_json(ordinary, asset_resolver));
                    result.expected_revision = decoded.expected_revision;
                    result.message = decoded.message;
                    result.entity_changes = decoded.entity_changes;
                } else result = std::get<ApplyBoundaryConstraintChanges>(command_from_json(proof, asset_resolver));
                if (result.expected_revision != command_revision(value.at("expected_revision"), "joint translation revision") ||
                    !value.at("message").is_string() || result.message != value.at("message").get<std::string>())
                    throw std::invalid_argument("Joint translation proof has a different revision or message");
                result.joint_translation_completion = true;
                result.joint_translation = std::move(joint_intent);
                if (per_target) {
                    const auto& presentations = value.at("presentation_proof");
                    if (!presentations.is_array() || presentations.size() > 1000 ||
                        !result.supplemental_entity_changes.empty())
                        throw std::invalid_argument("Joint presentation proof must be bounded and separate from ordinary geometry");
                    result.supplemental_entity_changes = std::get<ApplyEntityChanges>(command_from_json(nlohmann::json{
                        {"version", 1}, {"kind", "apply_entity_changes"}, {"expected_revision", result.expected_revision},
                        {"message", result.message}, {"entity_changes", presentations}, {"asset_changes", nlohmann::json::array()}})).entity_changes;
                    result.supplemental_source_completion = result.supplemental_source_completion ||
                        !result.supplemental_entity_changes.empty();
                    validate_joint_presentation_proof(result);
                }
                (void)command_to_json(Command{result});
                return result;
            }
            if (value.at("version") == 16) {
                command_exact_fields(value,{"version","kind","expected_revision","message","entity_changes","boundary_edits","wall_edits",
                    "physical_entity_changes","exterior_source_edits","supplemental_entity_changes","supplemental_asset_changes",
                    "source_completion","supplemental_source_completion","supplemental_asset_reference_completion",
                    "rigid_wall_transform_completion","measured_source_completion","measured_stroke_edits","exterior_corner_move",
                    "dimension_placement_completion","dimension_placement_moves","rigid_group_completion","rigid_group_transform"},
                    DocumentErrorCode::invalid_entity,"serialized mixed rigid group command");
                if (value.dump().size() > 1024*1024 || !value.at("rigid_group_completion").is_boolean() ||
                    !value.at("rigid_group_completion").get<bool>() || !value.at("dimension_placement_completion").is_boolean() ||
                    !value.at("dimension_placement_moves").is_array())
                    throw std::invalid_argument("Mixed rigid group mode or proof budget is invalid");
                const bool placement = value.at("dimension_placement_completion").get<bool>();
                if (!placement && !value.at("dimension_placement_moves").empty())
                    throw std::invalid_argument("Mixed placement mode disagrees with its typed lane");
                auto lanes = value; lanes["version"] = 15; lanes["dimension_placement_completion"] = true;
                lanes.erase("rigid_group_completion"); lanes.erase("rigid_group_transform");
                auto result = std::get<ApplyBoundaryConstraintChanges>(command_from_json(lanes, asset_resolver));
                result.dimension_placement_completion = placement;
                result.rigid_group_completion = true;
                const auto& child = value.at("rigid_group_transform");
                if (!child.is_null()) {
                    // Inspect identity before recursive decoding: no arbitrary
                    // commands or nested mixed envelopes. Qualified child
                    // dialects retain their own source-format floors.
                    if (!child.is_object() || !child.contains("kind") || child.at("kind") != "transform_boundaries" ||
                        !child.contains("version") || !child.at("version").is_number_integer() ||
                        (child.at("version") != 1 && child.at("version") != 2 && child.at("version") != 3))
                        throw std::invalid_argument("Mixed rigid child must be a TransformBoundaries proof");
                    result.rigid_group_transform = std::get<TransformBoundaries>(command_from_json(child, asset_resolver));
                }
                (void)command_to_json(Command{result});
                return result;
            }
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
                    if(!rigid_envelope && std::any_of(result.wall_edits.begin(),result.wall_edits.end(),[](const auto& edit){return edit.version==4 || edit.version==5;}))
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
                        if (!edit.is_object() || !edit.contains("version") ||
                            (edit.at("version") != 3 && edit.at("version") != 8))
                            throw std::invalid_argument("Automatic exterior source updates require typed retained-topology redraws");
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
    return history().at(static_cast<std::size_t>(revision_)).entities;
}
const std::map<std::string, Asset, std::less<>>& DocumentSnapshot::assets() const noexcept {
    return history().at(static_cast<std::size_t>(revision_)).assets;
}
const std::vector<RevisionRecord>& DocumentSnapshot::history() const noexcept {
    static const std::vector<RevisionRecord> empty;
    return history_ ? *history_ : empty;
}
bool DocumentSnapshot::uses_active_phase_constraints() const {
    const auto policies=active_constraint_history_policies(history());
    if (policies.empty()) return false;
    if (revision_>=policies.size())
        document_error(DocumentErrorCode::invalid_history,"Constraint policy revision is absent");
    return policies[static_cast<std::size_t>(revision_)];
}
bool DocumentSnapshot::shares_authoring_source_with(const DocumentSnapshot& other) const noexcept {
    return history_ && history_ == other.history_ && document_id_ == other.document_id_ &&
        revision_ == other.revision_ && named_revisions_ == other.named_revisions_;
}
bool DocumentSnapshot::shares_full_snapshot_with(const DocumentSnapshot& other) const noexcept {
    return shares_authoring_source_with(other) && saved_revision_ == other.saved_revision_ &&
        editable_ == other.editable_ && read_only_reason_ == other.read_only_reason_;
}
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
    document.stair_identity_history_.reserve_state(initial.entities);
    document.history_.push_back(std::move(initial));
    document.update_editability();
    return document;
}

Document Document::create_phase_import(std::vector<Entity> initial_entities, std::vector<Asset> initial_assets) {
    ImportPhaseEntities command;
    for (auto& entity:initial_entities) {
        if (entity.type=="model_phases") command.registry_ids.push_back(entity.id);
        command.entity_changes.push_back(EntityChange::upsert(std::move(entity)));
    }
    for (auto& asset:initial_assets) command.asset_changes.push_back(AssetChange::upsert(std::move(asset)));
    auto document=create();
    document.apply(command);
    return document;
}

const std::string& Document::document_id() const noexcept { return document_id_; }
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
    if (!snapshot_history_cache_)
        snapshot_history_cache_ = std::make_shared<const std::vector<RevisionRecord>>(history_);
    snapshot.history_ = snapshot_history_cache_;
    snapshot.named_revisions_ = named_revisions_;
    return snapshot;
}

bool Document::shares_authoring_source_with(const DocumentSnapshot& source) const noexcept {
    return snapshot_history_cache_ && snapshot_history_cache_ == source.history_ &&
        document_id_ == source.document_id_ && head_revision_ == source.revision_ &&
        named_revisions_ == source.named_revisions_;
}

Document Document::fork(const DocumentSnapshot& source) {
    return restore(source);
}

Document Document::fork_at_revision(const DocumentSnapshot& source, Revision revision) {
    (void)fork(source);
    if (revision >= source.history().size())
        document_error(DocumentErrorCode::invalid_history, "requested revision is not retained");
    auto prefix = source;
    prefix.history_ = std::make_shared<const std::vector<RevisionRecord>>(
        source.history().begin(), source.history().begin() + static_cast<std::size_t>(revision) + 1);
    prefix.revision_ = revision;
    if (prefix.saved_revision_ && *prefix.saved_revision_ > revision) prefix.saved_revision_.reset();
    for (auto it = prefix.named_revisions_.begin(); it != prefix.named_revisions_.end();) {
        if (it->second > revision) it = prefix.named_revisions_.erase(it);
        else ++it;
    }
    return restore(std::move(prefix));
}

struct PreparedDocumentEdit::State {
    explicit State(const DocumentSnapshot& captured) : source(captured) {}
    DocumentSnapshot source;
    std::string source_digest;
    std::unique_ptr<Document> candidate;
    bool consumed = false;
};

PreparedDocumentEdit::PreparedDocumentEdit(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
PreparedDocumentEdit::PreparedDocumentEdit(PreparedDocumentEdit&&) noexcept = default;
PreparedDocumentEdit& PreparedDocumentEdit::operator=(PreparedDocumentEdit&&) noexcept = default;
PreparedDocumentEdit::~PreparedDocumentEdit() = default;

DocumentSnapshot PreparedDocumentEdit::preview() const {
    if (!state_ || state_->consumed || !state_->candidate)
        throw std::invalid_argument("document edit is consumed or moved from");
    return state_->candidate->snapshot();
}

PreparedDocumentEdit Document::prepare_edit(const DocumentSnapshot& source, const Command& command) {
    auto state = std::make_unique<PreparedDocumentEdit::State>(source);
    state->candidate = std::make_unique<Document>(fork(source));
    state->source_digest = document_snapshot_digest(source);
    state->candidate->apply(command);
    return PreparedDocumentEdit(std::move(state));
}

Revision Document::commit_prepared(PreparedDocumentEdit& edit) {
    if (!edit.state_ || edit.state_->consumed || !edit.state_->candidate)
        throw std::invalid_argument("document edit is consumed or moved from");
    auto& state = *edit.state_;
    if (!editable_) document_error(DocumentErrorCode::read_only, read_only_reason_);
    {
        const auto current = snapshot();
        // Shared immutable history is a complete proof, including navigation,
        // typed envelopes and actual asset bytes. Independently retained equal
        // snapshots require the full digest; ID/head alone never authorize.
        if (!current.shares_full_snapshot_with(state.source) &&
            document_snapshot_digest(current) != state.source_digest)
            document_error(DocumentErrorCode::stale_revision, "prepared document edit source is stale");
    }
    const Revision revision = state.candidate->revision();
    std::swap(*this, *state.candidate);
    state.consumed = true;
    // Retain old state in the consumed token. Disposal does not belong in the
    // publication step and must be serialized with all token access.
    return revision;
}

DocumentSnapshot Document::preview_command(const DocumentSnapshot& source, const Command& command) {
    auto candidate = fork(source);
    candidate.apply(command);
    return candidate.snapshot();
}

Command complete_disto_measurement_command(
    const DocumentSnapshot& source, const Command& geometry_command,
    std::string_view owner_id, const DistoMeasurementRecord& record, bool replace_existing) {
    const auto* ordinary = std::get_if<ApplyEntityChanges>(&geometry_command);
    const auto* constrained = std::get_if<ApplyBoundaryConstraintChanges>(&geometry_command);
    if ((!ordinary && !constrained) || (constrained &&
        (constrained->wall_merge || (constrained->wall_split &&
            (constrained->wall_split->physical_room_completion || !constrained->wall_split->physical_room_owners.empty())) ||
         has_disto_measurement_completion(*constrained))))
        document_error(DocumentErrorCode::invalid_entity, "DISTO attachment requires one original geometry command");
    const DistoMeasurementAttachment attachment{std::string(owner_id), record, replace_existing};
    const auto geometry = Document::preview_command(source, geometry_command);
    auto completed = geometry.entities();
    try { attach_disto_measurement(source.entities(), completed, attachment,
        constrained?disto_completed_owner_id(*constrained,attachment):attachment.owner_id); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity, error.what()); }
    Command result = geometry_command;
    if (auto* changes = std::get_if<ApplyEntityChanges>(&result)) {
        const auto existing = std::find_if(changes->entity_changes.begin(), changes->entity_changes.end(), [&](const auto& change) {
            return (change.kind == EntityChangeKind::upsert ? change.entity.id : change.entity_id) == owner_id;
        });
        auto updated = EntityChange::upsert(completed.at(attachment.owner_id));
        if (existing == changes->entity_changes.end()) changes->entity_changes.push_back(std::move(updated));
        else *existing = std::move(updated);
    } else {
        auto& changes = std::get<ApplyBoundaryConstraintChanges>(result);
        changes.disto_measurement = attachment;
        changes.disto_measurement_completion = true;
    }
    const auto verified = Document::preview_command(source, result);
    if (verified.entities() != completed || verified.assets() != geometry.assets())
        document_error(DocumentErrorCode::invalid_entity, "DISTO attachment changed unrelated completed state");
    return result;
}

Command complete_selection_command(const DocumentSnapshot& source, const Command& geometry_command,
    const std::vector<EntityChange>& ordinary_changes, std::string message) {
    const auto* original = std::get_if<ApplyBoundaryConstraintChanges>(&geometry_command);
    if (!original) document_error(DocumentErrorCode::invalid_entity, "Selection completion requires a typed geometry command");
    validate_expected_revision(source.revision(), original->expected_revision);
    if (has_selection_completion(*original) && !original->selection_completion)
        document_error(DocumentErrorCode::invalid_entity, "Selection completion cannot repair a missing retained marker");
    if (ordinary_changes.size() > 1000)
        document_error(DocumentErrorCode::invalid_entity, "Selection completion exceeds its input target budget");
    auto completed = *original;
    if (!message.empty()) completed.message = std::move(message);
    completed.selection_completion = true;
    // Extensions retain one lane. A repeated exact consequence is harmless;
    // a second different payload for an existing target cannot overwrite it.
    std::unordered_set<std::string> submitted;
    for (const auto& change : ordinary_changes) {
        if (change.kind != EntityChangeKind::upsert)
            document_error(DocumentErrorCode::invalid_entity, "Selection completion cannot delete objects");
        if (!submitted.insert(change.entity.id).second)
            document_error(DocumentErrorCode::duplicate_change, "Selection completion repeats an ordinary target: " + change.entity.id);
        const auto found = std::find_if(completed.selection_entity_changes.begin(), completed.selection_entity_changes.end(),
            [&](const auto& retained) { return retained.entity.id == change.entity.id; });
        if (found == completed.selection_entity_changes.end()) completed.selection_entity_changes.push_back(change);
        else if (found->kind != change.kind || !exact_entity_payload(found->entity,change.entity))
            document_error(DocumentErrorCode::duplicate_change, "Selection completion has conflicting ordinary payloads: " + change.entity.id);
        if (completed.selection_entity_changes.size() > 1000)
            document_error(DocumentErrorCode::invalid_entity, "Selection completion exceeds its target budget");
    }
    try { validate_selection_changes(source.entities(), completed); }
    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
    const Command result{std::move(completed)};
    (void)Document::preview_command(source, result);
    return result;
}

Revision Document::apply(const Command& command) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    return std::visit(
        [this](const auto& typed_command) -> Revision {
            validate_expected_revision(head_revision_, typed_command.expected_revision);
            const auto& current = head_record();
            DxfReceiptValidationCache receipt_cache;
            // This private head has already passed complete state admission.
            // Reuse its proof only while exact receipt fields and private
            // immutable buffers match; newly supplied bytes are revalidated.
            validate_dxf_source_receipts(current.entities, current.assets, &receipt_cache, true);
            const bool source_active_policy=active_constraint_history_policies(history_).at(static_cast<std::size_t>(head_revision_));
            bool next_active_policy=source_active_policy;
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
            if constexpr (std::is_same_v<CommandType, ApplyEntityChanges> || std::is_same_v<CommandType, ImportPhaseEntities>) {
                if constexpr (std::is_same_v<CommandType, ImportPhaseEntities>)
                    next.action=typed_command.message.empty() ? "Import phase entities" : typed_command.message;
                else next.action = command_message(typed_command);
                validate_action(next.action);
                next.entities = ordinary_entity_changes(current.entities, typed_command.entity_changes);
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
                if constexpr (std::is_same_v<CommandType, ImportPhaseEntities>) {
                    PhaseEntityImportProof proof{typed_command.expected_revision,typed_command.message,typed_command.registry_ids,{},{},
                        typed_command.reviewed_existing_hierarchy_ids};
                    for (const auto& change:typed_command.entity_changes) {
                        if (change.kind!=EntityChangeKind::upsert)
                            document_error(DocumentErrorCode::invalid_entity,"Phase import cannot erase entities");
                        proof.entity_ids.push_back(change.entity.id);
                    }
                    for (const auto& change:typed_command.asset_changes) {
                        if (change.kind!=AssetChangeKind::upsert)
                            document_error(DocumentErrorCode::invalid_asset,"Phase import cannot erase assets");
                        proof.asset_ids.push_back(change.asset.id);
                    }
                    try { validate_phase_entity_import_transition(current,next,proof,history_); }
                    catch (const DocumentError&) { throw; }
                    catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_entity,error.what()); }
                    next.phase_entity_import=std::move(proof);
                    next_active_policy=true;
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
                if constexpr (std::is_same_v<CommandType, ImportPhaseEntities>)
                    if (next_unsupported_constraints)
                        document_error(DocumentErrorCode::invalid_entity,"Phase import requires supported source semantics: "+*next_unsupported_constraints);
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
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
                validate_constraint_change(current.entities, next.entities, false, true, true);
                if (same_state(next, current))
                    return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, TranslateBoundaries>) {
                next.action = typed_command.message.empty() ? "Translate boundaries" : typed_command.message;
                validate_action(next.action);
                next.boundary_translations = typed_command;
                next.entities = boundary_translation_entities(boundary_identity_history_, current.entities, typed_command);
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
                validate_constraint_change(current.entities, next.entities);
                if (same_state(next, current)) return head_revision_;
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else if constexpr (std::is_same_v<CommandType, ApplyBoundaryConstraintChanges>) {
                next.action = typed_command.message.empty()
                    ? "Apply boundary constraints" : typed_command.message;
                validate_action(next.action);
                next_active_policy=source_active_policy || !phase_constraint_authoring_proofs(typed_command).empty();
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                validate_phase_constraint_authoring_source(snapshot(),typed_command);
#endif
                if(typed_command.wall_split)validate_wall_split_lifetime(*typed_command.wall_split,history_,history_.size());
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                if (has_phase_room_review_completion(typed_command)) {
                    validate_phase_room_review_mode(typed_command);
                    const auto captured=snapshot();
                    const auto intent=decode_physical_wall_phase_room_review_intent(typed_command.phase_room_review_intent);
                    if (intent.expected_revision!=current.revision ||
                        intent.source_snapshot_digest!=document_snapshot_digest(captured) ||
                        intent.source_authoring_digest!=document_authoring_source_digest_v2(captured) ||
                        intent.source_saved_revision!=captured.saved_revision_optional() ||
                        intent.source_entities_digest!=entity_map_digest(current.entities))
                        document_error(DocumentErrorCode::stale_revision,"Phase room review source snapshot changed");
                    validate_phase_room_review_lifetime(typed_command.phase_room_review_intent,history_,history_.size());
                }
                if (has_room_review_completion(typed_command)) {
                    validate_room_review_mode(typed_command,true);
                    const auto captured=snapshot();
                    const auto snapshot_digest=document_snapshot_digest(captured);
                    const auto authoring_digest=document_authoring_source_digest_v2(captured);
                    for (const auto& encoded : room_review_intents(typed_command)) {
                        const auto intent=decode_physical_wall_room_review_intent(encoded);
                        if (intent.source_snapshot_digest!=snapshot_digest ||
                            intent.source_authoring_digest!=authoring_digest ||
                            intent.source_saved_revision!=captured.saved_revision_optional())
                            document_error(DocumentErrorCode::stale_revision,"Physical-room review source snapshot changed");
                        validate_room_review_lifetime(encoded,history_,history_.size());
                    }
                }
#endif
                next.boundary_constraint_changes = typed_command;
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                if (has_complete_wall_join_deletion_proof(typed_command))
                    validate_physical_wall_join_removal_identity_lifetime(snapshot(),complete_wall_join_deletion_destinations(typed_command));
#endif
                const std::map<std::string,Entity,std::less<>>* dimension_source=nullptr;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                dimension_source=room_dimension_original_source(history_,history_.size(),current.entities,typed_command);
#endif
                next.entities = completed_boundary_constraint_entities(boundary_identity_history_, current.entities, current.assets,
                    typed_command,false,source_active_policy,dimension_source);
                next.assets = boundary_constraint_assets(current.assets, typed_command);
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                if (!phase_constraint_authoring_proofs(typed_command).empty() || has_complete_wall_join_deletion_proof(typed_command))
                    validate_phase_constraint_fresh_lifetime(current.entities,next.entities,history_,history_.size(),typed_command);
#endif
                validate_completed_constraint_change(current.entities, next.entities, typed_command);
                try {
                    validate_boundary_identity_transition(
                        boundary_identity_history_, typed_command.wall_split ?
                            wall_split_validation_source(current.entities,next.entities,*typed_command.wall_split) :
                            typed_command.wall_merge ? wall_merge_validation_source(current.entities,next.entities,*typed_command.wall_merge) :
                            current.entities, next.entities);
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
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
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
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
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
                next_unsupported_constraints = validate_state(next.entities, next.assets,next_active_policy, &receipt_cache);
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

            validate_current_baseline_physical_preservation(current.entities,next.entities);
            // Source-bound rooms cannot borrow an ordinary metadata edit to
            // detach their holes or their physical-wall evidence. Deletion is
            // explicit; supported source refresh will need typed authority.
            validate_dxf_source_receipt_transition(current.entities, current.assets, next.entities, next.assets);
            validate_physical_room_source_transition(current.entities, next.entities,
                next.boundary_geometry_edit ? &*next.boundary_geometry_edit : nullptr,
                next.boundary_constraint_changes ? &*next.boundary_constraint_changes : nullptr,next_active_policy);
            auto next_stair_identity_history = stair_identity_history_;
            try {
                next_stair_identity_history.initialize_if_needed(history_, current.entities, next.entities);
                if constexpr (!std::is_same_v<CommandType, NameRevision>)
                    next_stair_identity_history.validate_transition(current.entities, next.entities);
                next_stair_identity_history.reserve_state(next.entities);
            } catch (const std::exception& error) {
                document_error(DocumentErrorCode::invalid_entity, error.what());
            }
            history_.push_back(std::move(next));
            snapshot_history_cache_.reset();
            boundary_identity_history_ = std::move(next_identity_history);
            stair_identity_history_ = std::move(next_stair_identity_history);
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
    if (const auto unsupported = validate_state(target.entities, target.assets,
        active_constraint_history_policies(history_).at(static_cast<std::size_t>(target_revision))))
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
    auto next_stair_identity_history = stair_identity_history_;
    next_stair_identity_history.reserve_state(next.entities);
    history_.push_back(std::move(next));
    snapshot_history_cache_.reset();
    stair_identity_history_ = std::move(next_stair_identity_history);
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
    if (const auto unsupported = validate_state(target.entities, target.assets,
        active_constraint_history_policies(history_).at(static_cast<std::size_t>(target_revision))))
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
    auto next_stair_identity_history = stair_identity_history_;
    next_stair_identity_history.reserve_state(next.entities);
    history_.push_back(std::move(next));
    snapshot_history_cache_.reset();
    stair_identity_history_ = std::move(next_stair_identity_history);
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

void Document::clear_saved_revision() noexcept {
    saved_revision_.reset();
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
    if (!snapshot.history_ || snapshot.history().empty() || snapshot.revision_ + 1 != snapshot.history().size()) {
        document_error(DocumentErrorCode::invalid_history, "stored revision history is incomplete");
    }
    if (snapshot.saved_revision_.has_value() && *snapshot.saved_revision_ > snapshot.revision_) {
        document_error(DocumentErrorCode::invalid_history, "stored saved revision is invalid");
    }
    std::map<std::string, Revision, std::less<>> expected_names;
    std::optional<std::string> unsupported_constraint_history;
    BoundaryIdentityHistory identity_history;
    StairIdentityHistory stair_identity_history;
    DxfReceiptValidationCache receipt_cache;
    const auto active_policies=active_constraint_history_policies(snapshot.history());
    for (std::size_t index = 0; index < snapshot.history().size(); ++index) {
        const auto& record = snapshot.history()[index];
        validate_action(record.action);
        if (record.revision != index) {
            document_error(DocumentErrorCode::invalid_history, "stored revisions are not contiguous");
        }
        auto unsupported = validate_state(record.entities, record.assets,active_policies.at(index), &receipt_cache);
        if (unsupported && !unsupported_constraint_history)
            unsupported_constraint_history = std::move(unsupported);

        if (index == 0) {
            if (record.parent_revision.has_value() || record.source_revision.has_value() || record.boundary_translation.has_value() ||
                record.boundary_transform.has_value() || record.boundary_geometry_edit.has_value() ||
                record.boundary_constraint_changes.has_value() || record.boundary_translations.has_value() || record.boundary_transforms.has_value() ||
                record.phase_entity_import.has_value() ||
                record.name.has_value() || record.action != "create" ||
                !record.undo_stack.empty() || !record.redo_stack.empty()) {
                document_error(DocumentErrorCode::invalid_history,
                               "revision zero is not a valid create record");
            }
            record_boundary_identities(identity_history, record.entities);
            stair_identity_history.reserve_state(record.entities);
            continue;
        }

        const auto& previous = snapshot.history()[index - 1];
        if (!record.source_revision)
            validate_dxf_source_receipt_transition(previous.entities, previous.assets, record.entities, record.assets);
        const auto boundary_proof_count =
            static_cast<unsigned>(record.boundary_translation.has_value()) +
            static_cast<unsigned>(record.boundary_transform.has_value()) +
            static_cast<unsigned>(record.boundary_geometry_edit.has_value()) +
            static_cast<unsigned>(record.boundary_constraint_changes.has_value()) +
            static_cast<unsigned>(record.boundary_translations.has_value()) +
            static_cast<unsigned>(record.boundary_transforms.has_value()) +
            static_cast<unsigned>(record.phase_entity_import.has_value());
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
        if (record.phase_entity_import) {
            if (record.name || record.source_revision ||
                record.action!=(record.phase_entity_import->message.empty() ? "Import phase entities" : record.phase_entity_import->message))
                document_error(DocumentErrorCode::invalid_history,"Phase import proof does not match its command event");
            try {
                validate_phase_entity_import_transition(previous,record,*record.phase_entity_import,
                    std::span<const RevisionRecord>(snapshot.history().data(),index));
            } catch (const std::exception& error) { document_error(DocumentErrorCode::invalid_history,error.what()); }
            if (unsupported_constraint_history)
                document_error(DocumentErrorCode::invalid_history,"Phase import cannot descend from unsupported source semantics");
        }
        // Unknown locks retain the read-only latch, but must not suppress
        // stable-endpoint checks for known relations in the same history.
        // Undo/redo restores an exact retained state and its provenance. The
        // source-state and stack checks below validate navigation; mutation
        // rules must not reject restoration of a shorter derivation prefix.
        if (record.boundary_constraint_changes && (record.boundary_constraint_changes->wall_split || record.boundary_constraint_changes->wall_merge || has_exterior_source_completion(*record.boundary_constraint_changes) ||
            has_rigid_wall_transform(*record.boundary_constraint_changes) || has_rigid_group_completion(*record.boundary_constraint_changes) ||
            has_joint_translation_completion(*record.boundary_constraint_changes) || has_room_review_completion(*record.boundary_constraint_changes) ||
            has_phase_room_review_completion(*record.boundary_constraint_changes) ||
            has_phase_constraint_authoring(*record.boundary_constraint_changes) ||
            has_disto_measurement_completion(*record.boundary_constraint_changes) || has_selection_completion(*record.boundary_constraint_changes) ||
            has_curve_construction_completion(*record.boundary_constraint_changes)))
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
            const auto& source = snapshot.history()[static_cast<std::size_t>(*record.source_revision)];
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
        // Exact history navigation may undo a protected boundary identity
        // upgrade. Authored boundary changes cannot claim that authority.
        if (!record.source_revision.has_value()) {
            if (!record.name) {
                try {
                    stair_identity_history.initialize_if_needed(
                        std::span<const RevisionRecord>(snapshot.history().data(), index),
                        previous.entities, record.entities);
                    stair_identity_history.validate_transition(previous.entities, record.entities);
                } catch (const std::exception& error) {
                    document_error(DocumentErrorCode::invalid_history, error.what());
                }
            }
            validate_physical_room_source_transition(previous.entities, record.entities,
                record.boundary_geometry_edit ? &*record.boundary_geometry_edit : nullptr,
                record.boundary_constraint_changes ? &*record.boundary_constraint_changes : nullptr,active_policies.at(index));
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
                    validate_split_dimension_lifetime(*record.boundary_geometry_edit, snapshot.history(), index);
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
                    if(proof.wall_split)validate_wall_split_lifetime(*proof.wall_split,snapshot.history(),index);
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                    validate_retained_phase_constraint_authoring_source(snapshot,previous,proof,index);
#endif
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                    if (has_phase_room_review_completion(proof)) {
                        validate_phase_room_review_mode(proof);
                        const auto intent=decode_physical_wall_phase_room_review_intent(proof.phase_room_review_intent);
                        if (intent.expected_revision!=previous.revision ||
                            intent.source_authoring_digest!=document_authoring_source_digest_v2_at_revision(snapshot,previous.revision) ||
                            intent.source_snapshot_digest!=document_snapshot_digest_at_revision(snapshot,previous.revision,intent.source_saved_revision) ||
                            intent.source_entities_digest!=entity_map_digest(previous.entities))
                            throw std::invalid_argument("Phase room review retained source authority changed");
                        validate_phase_room_review_lifetime(proof.phase_room_review_intent,snapshot.history(),index);
                    }
                    if (has_room_review_completion(proof)) {
                        validate_room_review_mode(proof,true);
                        const auto first=decode_physical_wall_room_review_intent(proof.room_review_intent);
                        const auto authoring_digest=document_authoring_source_digest_v2_at_revision(snapshot,previous.revision);
                        const auto snapshot_digest=document_snapshot_digest_at_revision(snapshot,previous.revision,first.source_saved_revision);
                        for (const auto& encoded : room_review_intents(proof)) {
                            const auto intent=decode_physical_wall_room_review_intent(encoded);
                            if (intent.source_authoring_digest!=authoring_digest || intent.source_snapshot_digest!=snapshot_digest ||
                                intent.source_saved_revision!=first.source_saved_revision)
                                throw std::invalid_argument("Physical-room review retained source authority changed");
                            validate_room_review_lifetime(encoded,snapshot.history(),index);
                        }
                    }
#endif
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                    if (has_complete_wall_join_deletion_proof(proof))
                        validate_physical_wall_join_removal_identity_lifetime(previous.entities,snapshot.history(),index,complete_wall_join_deletion_destinations(proof));
#endif
                    const std::map<std::string,Entity,std::less<>>* dimension_source=nullptr;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                    dimension_source=room_dimension_original_source(snapshot.history(),index,previous.entities,proof);
#endif
                    expected.entities = completed_boundary_constraint_entities(identity_history, previous.entities, previous.assets,
                        proof,true,active_policies.at(index-1),dimension_source);
#ifdef VERTEX_HAS_CONSTRAINT_AUTHORING
                    if (!phase_constraint_authoring_proofs(proof).empty() || has_complete_wall_join_deletion_proof(proof))
                        validate_phase_constraint_fresh_lifetime(previous.entities,expected.entities,snapshot.history(),index,proof);
#endif
                    expected.assets = boundary_constraint_assets(previous.assets, proof);
                    validate_boundary_identity_transition(identity_history, proof.wall_split ?
                        wall_split_validation_source(previous.entities,expected.entities,*proof.wall_split) :
                        proof.wall_merge ? wall_merge_validation_source(previous.entities,expected.entities,*proof.wall_merge) :
                        previous.entities, record.entities);
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
        try {
            stair_identity_history.reserve_state(record.entities);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_history, error.what());
        }
    }
    if (expected_names != snapshot.named_revisions_) {
        document_error(DocumentErrorCode::invalid_history,
                       "named revision index is not a bijection with history");
    }
    Document document(std::move(snapshot.document_id_));
    document.head_revision_ = snapshot.revision_;
    document.saved_revision_ = snapshot.saved_revision_;
    document.history_ = snapshot.history();
    document.snapshot_history_cache_ = std::move(snapshot.history_);
    document.named_revisions_ = std::move(snapshot.named_revisions_);
    document.unsupported_constraint_history_reason_ = std::move(unsupported_constraint_history);
    if (!snapshot.editable_ && !snapshot.read_only_reason_.empty()) {
        document.session_read_only_reason_ = snapshot.read_only_reason_;
    }
    document.boundary_identity_history_ = std::move(identity_history);
    document.stair_identity_history_ = std::move(stair_identity_history);
    document.update_editability();
    return document;
}

}  // namespace sketch
