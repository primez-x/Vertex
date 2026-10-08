#include "sketch/architectural_footprint_edit.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_solid.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

DocumentRoomFootprint read_footprint(const Entity& entity) {
    DocumentRoomFootprint footprint;
    std::string error;
    if (entity.type == "slab") {
        Slab slab;
        if (!read_document_slab(entity, slab, error))
            throw std::invalid_argument("Slab " + entity.id + ": " + error);
        footprint = {std::move(slab.boundary), std::move(slab.holes)};
        if (const auto invalid = validate_boundary_holes(footprint.boundary, footprint.holes))
            throw std::invalid_argument("Slab " + entity.id + ": " + *invalid);
    } else if (!read_document_room_footprint(entity, footprint, error)) {
        throw std::invalid_argument("Room " + entity.id + ": " + error);
    }
    return footprint;
}

bool same_boundary(const Boundary& first, const Boundary& second) {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const auto& a = first[index];
        const auto& b = second[index];
        if (a.start.x != b.start.x || a.start.y != b.start.y ||
            a.end.x != b.end.x || a.end.y != b.end.y ||
            a.sweep_radians != b.sweep_radians) return false;
    }
    return true;
}

void update_point(nlohmann::json& point, Vec2 target) {
    // The admitted solid codecs require exactly two numeric array entries.
    // Keep unchanged numbers in their original JSON representation too.
    if (point.at(0).get<double>() != target.x) point.at(0) = target.x;
    if (point.at(1).get<double>() != target.y) point.at(1) = target.y;
}

void update_ring_vertex(nlohmann::json& ring, std::size_t index, Vec2 target) {
    const auto preceding = index == 0 ? ring.size() - 1 : index - 1;
    update_point(ring.at(index).at("start"), target);
    update_point(ring.at(preceding).at("end"), target);
}

void require_scalar_alias_agreement(const Entity& entity, const char* canonical,
                                    const char* legacy) {
    const auto& properties = entity.properties;
    if (!properties.contains(canonical) || !properties.contains(legacy)) return;
    const auto& first = properties.at(canonical);
    const auto& second = properties.at(legacy);
    if (!first.is_number() || !second.is_number() ||
        !std::isfinite(first.get<double>()) || !std::isfinite(second.get<double>()) ||
        first.get<double>() != second.get<double>())
        throw std::invalid_argument(std::string("Footprint scalar aliases disagree: ") +
                                    canonical + " and " + legacy);
}

void admit_detached_volume(const Entity& entity) {
    std::string error;
    if (entity.type == "slab") {
        Slab slab;
        if (!read_document_slab(entity, slab, error))
            throw std::invalid_argument("Slab " + entity.id + ": " + error);
        (void)make_slab(slab);
    } else if (has_document_room_volume_fields(entity)) {
        RoomVolume room;
        if (!read_document_room(entity, room, error))
            throw std::invalid_argument("Room " + entity.id + ": " + error);
    }
}

}  // namespace

Entity stage_architectural_footprint_vertex_entity(
    const Entity& original, const FootprintVertexEdit& edit) {
    if (original.type != "slab" && original.type != "room")
        throw DocumentError(DocumentErrorCode::invalid_entity, "Footprint edit requires an independent slab or room");
    if (!std::isfinite(edit.proposed_position.x) || !std::isfinite(edit.proposed_position.y))
        throw std::invalid_argument("Footprint vertex target must be finite");
    if (original.extensions.contains("physical_wall_room") ||
        original.properties.contains("wall_measurement_source")) {
        throw std::invalid_argument("Edit a wall-derived room footprint through its source walls");
    }
    require_scalar_alias_agreement(original,
        original.type == "slab" ? "thickness_m" : "height_m",
        original.type == "slab" ? "thickness" : "height");
    require_scalar_alias_agreement(original, "elevation_m", "elevation");
    const auto footprint = read_footprint(original);
    admit_detached_volume(original);
    const Boundary* ring = &footprint.boundary;
    if (edit.hole_index) {
        if (*edit.hole_index >= footprint.holes.size())
            throw std::invalid_argument("Footprint hole index is outside the captured source");
        ring = &footprint.holes.at(*edit.hole_index);
    }
    if (edit.vertex_index >= ring->size())
        throw std::invalid_argument("Footprint vertex index is outside the captured ring");
    const auto vertex = ring->at(edit.vertex_index).start;

    const bool room_aliases = original.type == "room" &&
        original.properties.contains("boundary") && original.properties.contains("segments");
    if (room_aliases) {
        // Match the existing canonical-first reader while preventing the edit
        // from silently replacing a conflicting retained alias's geometry.
        auto legacy = original;
        legacy.properties.erase("boundary");
        if (!same_boundary(footprint.boundary, read_footprint(legacy).boundary))
            throw std::invalid_argument("Room footprint boundary and segments aliases disagree");
    }

    if (vertex.x == edit.proposed_position.x && vertex.y == edit.proposed_position.y) {
        return original;
    }

    auto entity = original;
    if (edit.hole_index) {
        update_ring_vertex(entity.properties.at("holes").at(*edit.hole_index),
                           edit.vertex_index, edit.proposed_position);
    } else {
        const auto* field = entity.properties.contains("boundary") ? "boundary" : "segments";
        update_ring_vertex(entity.properties.at(field), edit.vertex_index, edit.proposed_position);
        if (room_aliases)
            update_ring_vertex(entity.properties.at("segments"), edit.vertex_index, edit.proposed_position);
    }
    // Re-read the exact returned payload, including every untouched hole and
    // arc, before detached physical admission. The command performs resolved
    // Document admission separately using its actual snapshot.
    (void)read_footprint(entity);
    admit_detached_volume(entity);
    return entity;
}

ApplyEntityChanges architectural_footprint_vertex_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    const FootprintVertexEdit& edit, Revision expected_revision) {
    if (source.revision() != expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision, "Footprint edit source revision is stale");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    const auto found = source.entities().find(entity_id);
    if (found == source.entities().end())
        throw DocumentError(DocumentErrorCode::dangling_reference, "Footprint edit target is missing");
    const auto& original = found->second;
    auto entity = stage_architectural_footprint_vertex_entity(original, edit);
    if (entity == original)
        throw std::invalid_argument("Footprint vertex target makes no document change");
    ApplyEntityChanges command{expected_revision, {EntityChange::upsert(std::move(entity))}, {},
                               "Move footprint vertex"};
    const auto candidate = Document::preview_command(source, Command{command});
    if (original.type == "slab")
        validate_architectural_geometry_changes(source, candidate, {entity_id});
    else
        validate_architectural_geometry_changes(source, candidate);
    return command;
}

}  // namespace sketch
