#include "sketch/room_relationship_geometry_commit.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace sketch {
namespace {

const char* entity_type(RoomReferenceKind kind) {
    switch (kind) {
    case RoomReferenceKind::room_boundary: return "room_boundary";
    case RoomReferenceKind::appraisal_measurement_boundary: return "measurement_boundary";
    case RoomReferenceKind::architectural_wall: return "wall";
    }
    throw std::invalid_argument("Unknown room reference kind");
}

bool same_geometry(const Boundary& left, const Boundary& right,
                   double tolerance = default_geometry_tolerance_metres) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto distance = [](Vec2 a, Vec2 b) {
            return std::hypot(a.x - b.x, a.y - b.y);
        };
        if (distance(left[index].start, right[index].start) > tolerance ||
            distance(left[index].end, right[index].end) > tolerance ||
            std::abs(left[index].sweep_radians - right[index].sweep_radians) >
                std::max(1e-12, tolerance)) {
            return false;
        }
    }
    return true;
}

nlohmann::json point_json(Vec2 point) {
    return nlohmann::json::array({point.x, point.y});
}

void update_segment_json(nlohmann::json& target, const Segment& segment) {
    if (!target.is_object()) target = nlohmann::json::object();
    target["start"] = point_json(segment.start);
    target["end"] = point_json(segment.end);
    target["sweep_radians"] = segment.sweep_radians;
}

struct GeometryCollection {
    std::vector<RelationshipGeometry> records;
    std::vector<std::string> diagnostics;
};

void add_diagnostic(std::vector<std::string>& diagnostics,
                    std::set<std::string, std::less<>>& seen,
                    std::string message) {
    if (seen.insert(message).second) diagnostics.push_back(std::move(message));
}

GeometryCollection snapshot_geometry(const DocumentSnapshot& source,
                                      const RoomRelationshipSnapshot& relationships) {
    GeometryCollection result;
    std::set<std::string, std::less<>> seen;
    for (const auto& reference : relationships.references()) {
        const auto found = source.entities().find(reference.id);
        if (found == source.entities().end()) {
            add_diagnostic(result.diagnostics, seen,
                           "missing relationship geometry entity " + reference.id);
            continue;
        }
        if (found->second.type != entity_type(reference.kind)) {
            add_diagnostic(result.diagnostics, seen,
                           "relationship entity " + reference.id + " has type " +
                           found->second.type + ", expected " + entity_type(reference.kind));
            continue;
        }
        try {
            if (reference.kind == RoomReferenceKind::architectural_wall) {
                std::vector<const Entity*> openings;
                for (const auto& [id, entity] : source.entities()) {
                    (void)id;
                    if (entity.type != "opening") continue;
                    std::string wall_id;
                    std::string diagnostic;
                    if (read_document_wall_id(entity, wall_id, diagnostic) &&
                        wall_id == reference.id) {
                        openings.push_back(&entity);
                    }
                }
                Wall wall;
                std::string diagnostic;
                if (!read_document_wall(found->second, openings, wall, diagnostic)) {
                    add_diagnostic(result.diagnostics, seen,
                                   "invalid relationship wall " + reference.id + ": " + diagnostic);
                    continue;
                }
                validate_wall_semantics(wall);
                result.records.push_back({reference.id, reference.kind, {wall.baseline}});
            } else {
                result.records.push_back({reference.id, reference.kind,
                                          boundary_geometry(decode_identified_boundary_entity(
                                              found->second))});
            }
        } catch (const std::exception& error) {
            add_diagnostic(result.diagnostics, seen,
                           "invalid relationship geometry " + reference.id + ": " + error.what());
        }
    }
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    return result;
}

Entity update_boundary_entity(const Entity& original,
                              const RelationshipGeometryChange& change) {
    auto boundary = decode_identified_boundary_entity(original);
    if (boundary.segments.size() != change.geometry.size()) {
        throw std::invalid_argument("relationship proposal changed boundary topology for " +
                                    original.id);
    }

    if (original.properties.contains("boundary_authoring")) {
        const auto decoded = decode_boundary_receipt_envelope(
            original.properties.at("boundary_authoring"));
        if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
        const auto transformed = transformed_boundary_construction(*decoded.record,
                                                                    change.transform);
        const auto replay = replay_boundary_construction(transformed);
        if (replay.edges.size() != change.geometry.size()) {
            throw std::invalid_argument("relationship receipt replay changed boundary topology for " +
                                        original.id);
        }
        IdentifiedBoundary replayed{original.id, original.type, {}};
        replayed.segments.reserve(replay.edges.size());
        for (const auto& edge : replay.edges) {
            replayed.segments.push_back({edge.segment_id, edge.start_vertex_id,
                                         edge.end_vertex_id, edge.segment});
        }
        if (!same_geometry(boundary_geometry(replayed), change.geometry)) {
            throw std::invalid_argument("relationship proposal does not match construction receipt replay for " +
                                        original.id);
        }
        auto metadata = original;
        metadata.properties.erase("boundary_authoring");
        auto encoded = encode_identified_boundary_entity(replayed, &metadata);
        encoded.properties["boundary_authoring"] = encode_boundary_receipt_envelope(transformed);
        return encoded;
    }

    for (std::size_t index = 0; index < boundary.segments.size(); ++index) {
        boundary.segments[index].segment = change.geometry[index];
    }
    return encode_identified_boundary_entity(boundary, &original);
}

Entity update_wall_entity(const DocumentSnapshot& source, const Entity& original,
                          const RelationshipGeometryChange& change) {
    if (change.geometry.size() != 1) {
        throw std::invalid_argument("relationship wall proposal must contain one baseline segment");
    }
    std::vector<const Entity*> openings;
    for (const auto& [id, entity] : source.entities()) {
        (void)id;
        if (entity.type != "opening") continue;
        std::string wall_id;
        std::string diagnostic;
        if (read_document_wall_id(entity, wall_id, diagnostic) && wall_id == original.id) {
            openings.push_back(&entity);
        }
    }
    Wall wall;
    std::string diagnostic;
    if (!read_document_wall(original, openings, wall, diagnostic)) {
        throw std::invalid_argument(diagnostic);
    }
    wall.baseline = change.geometry.front();
    validate_wall_semantics(wall);
    auto updated = original;
    update_segment_json(updated.properties["baseline"], wall.baseline);
    rebase_wall_length_receipt(updated, wall.baseline);
    return updated;
}

std::vector<EntityChange> build_entity_changes(
    const DocumentSnapshot& source,
    const std::vector<RelationshipGeometryChange>& changes,
    std::map<std::string, Entity, std::less<>>* candidate_entities) {
    std::map<std::string, Entity, std::less<>> updates;
    const auto add_update = [&](Entity entity) {
        if (!updates.emplace(entity.id, std::move(entity)).second) {
            throw std::invalid_argument("relationship proposal updates entity more than once");
        }
    };

    for (const auto& change : changes) {
        const auto found = source.entities().find(change.source_id);
        if (found == source.entities().end()) {
            throw std::invalid_argument("relationship proposal source entity is missing: " +
                                        change.source_id);
        }
        if (found->second.type != entity_type(change.source_kind)) {
            throw std::invalid_argument("relationship proposal source role does not match entity " +
                                        change.source_id);
        }
        if (change.source_kind == RoomReferenceKind::architectural_wall) {
            add_update(update_wall_entity(source, found->second, change));
        } else {
            add_update(update_boundary_entity(found->second, change));
        }
    }

    // Dimensions are dependent presentation objects. Move their text anchors
    // with the source while preserving the source segment identity and all
    // unrelated metadata. Unsupported dimensions fail the complete preview.
    for (const auto& change : changes) {
        if (change.source_kind == RoomReferenceKind::architectural_wall) continue;
        for (const auto& [id, entity] : source.entities()) {
            (void)id;
            if (entity.type != "dimension" || !entity.properties.contains("target")) continue;
            const auto& target = entity.properties.at("target");
            if (!target.is_object() || target.value("entity_id", std::string{}) != change.source_id)
                continue;
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
            auto dimension = *decoded.dimension;
            dimension.text_position = transform_point(dimension.text_position, change.transform);
            add_update(encode_boundary_dimension_entity(dimension, &entity));
        }
    }

    if (candidate_entities) {
        *candidate_entities = source.entities();
        for (const auto& [id, entity] : updates) (*candidate_entities)[id] = entity;
    }
    std::vector<EntityChange> result;
    result.reserve(updates.size());
    for (auto& [id, entity] : updates) {
        (void)id;
        result.push_back(EntityChange::upsert(std::move(entity)));
    }
    return result;
}

void merge_diagnostics(std::vector<std::string>& destination,
                       const std::vector<std::string>& additional) {
    std::set<std::string, std::less<>> seen(destination.begin(), destination.end());
    for (const auto& diagnostic : additional) {
        if (seen.insert(diagnostic).second) destination.push_back(diagnostic);
    }
    std::sort(destination.begin(), destination.end());
}

} // namespace

RoomRelationshipGeometrySnapshot snapshot_room_relationship_geometry(
    const DocumentSnapshot& source, const RoomRelationshipSnapshot& relationships) {
    auto snapshot = snapshot_geometry(source, relationships);
    return {std::move(snapshot.records), std::move(snapshot.diagnostics)};
}

RoomRelationshipGeometryPreview preview_room_relationship_geometry(
    const DocumentSnapshot& source,
    const RoomRelationshipSnapshot& relationships,
    const std::vector<RelationshipGeometry>& edited_after) {
    RoomRelationshipGeometryPreview preview;
    preview.document_id_ = source.document_id();
    preview.expected_revision_ = source.revision();
    preview.source_snapshot_digest_ = document_snapshot_digest(source);
    try {
        const auto before = snapshot_geometry(source, relationships);
        auto result = propose_room_relationship_geometry(relationships, before.records, edited_after);
        preview.changes_ = result.changes;
        preview.diagnostics_ = std::move(result.diagnostics);
        merge_diagnostics(preview.diagnostics_, before.diagnostics);
        if (preview.diagnostics_.empty() && preview.changes_.empty()) {
            preview.diagnostics_.push_back("no relationship geometry changes were proposed");
        }
        if (!preview.diagnostics_.empty()) return preview;

        std::map<std::string, Entity, std::less<>> candidate_entities;
        const auto entity_changes = build_entity_changes(source, preview.changes_,
                                                          &candidate_entities);
        const auto command = ApplyEntityChanges{source.revision(), entity_changes, {},
                                                "Propagate room relationships"};
        const auto candidate = Document::preview_command(source, Command{command});
        preview.candidate_entity_digest_ = entity_map_digest(candidate.entities());
        preview.accepted_ = true;
    } catch (const std::exception& error) {
        preview.accepted_ = false;
        preview.diagnostics_.push_back(error.what());
        merge_diagnostics(preview.diagnostics_, {});
        preview.changes_.clear();
    }
    return preview;
}

Revision apply_room_relationship_geometry(
    Document& document, const RoomRelationshipGeometryPreview& preview) {
    const auto command = make_room_relationship_geometry_command(document.snapshot(), preview);
    return document.apply(Command{command});
}

ApplyEntityChanges make_room_relationship_geometry_command(
    const DocumentSnapshot& source, const RoomRelationshipGeometryPreview& preview) {
    if (!preview.accepted_) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "cannot apply a rejected room relationship geometry preview");
    }
    if (source.document_id() != preview.document_id_ ||
        source.revision() != preview.expected_revision_ ||
        document_snapshot_digest(source) != preview.source_snapshot_digest_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "room relationship geometry preview does not match the current document");
    }
    try {
        std::map<std::string, Entity, std::less<>> candidate_entities;
        const auto entity_changes = build_entity_changes(source, preview.changes_,
                                                          &candidate_entities);
        const auto command = ApplyEntityChanges{source.revision(), entity_changes, {},
                                                "Propagate room relationships"};
        const auto candidate = Document::preview_command(source, Command{command});
        if (entity_map_digest(candidate.entities()) != preview.candidate_entity_digest_) {
            throw DocumentError(DocumentErrorCode::invalid_entity,
                                "room relationship geometry preview changed before commit");
        }
        return command;
    } catch (const DocumentError&) {
        throw;
    } catch (const std::exception& error) {
        throw DocumentError(DocumentErrorCode::invalid_entity, error.what());
    }
}

} // namespace sketch
