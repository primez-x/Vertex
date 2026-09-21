#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/geometry_operations.hpp"
#include <cmath>
#include <set>

namespace sketch {
namespace {
bool exact_entity(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
           left.extensions.dump() == right.extensions.dump();
}
bool identified_v1(const Entity& entity) {
    return can_recognize_boundary_entity_type(entity.type) &&
        inspect_boundary_entity_version(entity).format == BoundaryEntityFormat::identified_v1;
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

bool same_record_without_transforms(const BoundaryConstructionRecord& left,
                                    const BoundaryConstructionRecord& right) {
    return left.replay_version == right.replay_version && left.anchor.x == right.anchor.x &&
        left.anchor.y == right.anchor.y && left.boundary_id == right.boundary_id &&
        left.edges == right.edges && left.extensions == right.extensions;
}

bool valid_explicit_relationship_transform(const Entity& previous, const Entity& next) {
    if (!identified_v1(previous) || !identified_v1(next) ||
        !previous.properties.contains("boundary_authoring") ||
        !next.properties.contains("boundary_authoring")) {
        return false;
    }
    try {
        const auto old_receipt = decode_boundary_receipt_envelope(
            previous.properties.at("boundary_authoring"));
        const auto new_receipt = decode_boundary_receipt_envelope(
            next.properties.at("boundary_authoring"));
        if (!old_receipt.supported() || !new_receipt.supported()) return false;
        const auto& old_record = *old_receipt.record;
        const auto& new_record = *new_receipt.record;
        if (new_record.schema_version != boundary_receipt_schema_version_v3 ||
            new_record.transforms.empty() ||
            !same_record_without_transforms(old_record, new_record)) {
            return false;
        }
        if (old_record.schema_version == boundary_receipt_schema_version_v3) {
            if (new_record.transforms.size() != old_record.transforms.size() + 1) return false;
            for (std::size_t index = 0; index < old_record.transforms.size(); ++index) {
                if (!(new_record.transforms[index] == old_record.transforms[index])) return false;
            }
        } else if (new_record.transforms.size() != 1) {
            return false;
        }
        const auto old_replay = replay_boundary_construction(old_record);
        const auto new_replay = replay_boundary_construction(new_record);
        Boundary old_geometry;
        Boundary new_geometry;
        old_geometry.reserve(old_replay.edges.size());
        new_geometry.reserve(new_replay.edges.size());
        for (const auto& edge : old_replay.edges) old_geometry.push_back(edge.segment);
        for (const auto& edge : new_replay.edges) new_geometry.push_back(edge.segment);
        const auto next_boundary = boundary_geometry(decode_identified_boundary_entity(next));
        if (!same_geometry(new_geometry, next_boundary) ||
            new_geometry.size() != old_geometry.size()) return false;
        const auto& transform = new_record.transforms.back();
        Boundary transformed;
        transformed.reserve(old_geometry.size());
        for (const auto& segment : old_geometry) transformed.push_back(transform_segment(segment, transform));
        if (!same_geometry(transformed, new_geometry)) return false;

        auto previous_metadata = previous;
        auto next_metadata = next;
        previous_metadata.properties.erase("segments");
        previous_metadata.properties.erase("boundary_authoring");
        next_metadata.properties.erase("segments");
        next_metadata.properties.erase("boundary_authoring");
        return previous_metadata == next_metadata &&
            previous_metadata.properties.dump() == next_metadata.properties.dump() &&
            previous_metadata.extensions.dump() == next_metadata.extensions.dump();
    } catch (const std::exception&) {
        return false;
    }
}

IdentifiedBoundary apply_geometry_edit(const IdentifiedBoundary& source,
                                       const BoundaryGeometryEdit& edit) {
    validate_boundary_geometry_edit(edit);
    if (source.id != edit.boundary_id)
        throw std::invalid_argument("Boundary geometry edit owner does not match");
    if (edit.kind == BoundaryGeometryEditKind::move_vertex)
        return move_boundary_vertex(source, edit.target_id, edit.target_position);
    return set_boundary_segment_length(source, edit.target_id, edit.target_length_metres,
                                       edit.fixed_endpoint, edit.move_connected);
}

IdentifiedBoundary apply_geometry_transform(const IdentifiedBoundary& source,
                                             const BoundaryTransformation& transformation) {
    validate_boundary_transform(transformation);
    if (source.id != transformation.boundary_id)
        throw std::invalid_argument("Boundary transform owner does not match");
    auto result = source;
    for (auto& edge : result.segments) {
        edge.segment = transform_segment(edge.segment, transformation.transform);
    }
    (void)encode_identified_boundary_entity(result);
    return result;
}

nlohmann::json geometry_edit_operation(const BoundaryGeometryEdit& edit) {
    return {{"kind", "geometry_edit"}, {"value", encode_boundary_geometry_edit(edit)}};
}

IdentifiedBoundary apply_vertex_batch(const IdentifiedBoundary& source,
                                      const std::vector<BoundaryGeometryEdit>& edits) {
    if (edits.empty()) throw std::invalid_argument("Empty boundary vertex batch");
    auto result = source;
    std::set<std::string, std::less<>> touched;
    for (const auto& edit : edits) {
        validate_boundary_geometry_edit(edit);
        if (edit.boundary_id != source.id || edit.kind != BoundaryGeometryEditKind::move_vertex ||
            !touched.insert(edit.target_id).second)
            throw std::invalid_argument("Boundary vertex batch has invalid owner, kind or duplicate target");
        bool found = false;
        for (auto& edge : result.segments) {
            if (edge.segment.sweep_radians != 0)
                throw std::invalid_argument("Boundary vertex batch requires straight segments");
            if (edge.start_vertex_id == edit.target_id) {
                edge.segment.start = edit.target_position;
                found = true;
            }
            if (edge.end_vertex_id == edit.target_id) edge.segment.end = edit.target_position;
        }
        if (!found) throw std::invalid_argument("Unknown batch vertex ID");
    }
    (void)encode_identified_boundary_entity(result);
    if (signed_area(boundary_geometry(source)) * signed_area(boundary_geometry(result)) <= 0)
        throw std::invalid_argument("Boundary vertex batch must preserve winding");
    return result;
}

nlohmann::json geometry_transform_operation(const BoundaryTransformation& transformation) {
    return {{"kind", "transform"}, {"value", encode_boundary_transform(transformation)}};
}

IdentifiedBoundary replay_geometry_derivation(const Entity& entity) {
    const auto found = entity.extensions.find("boundary_geometry_derivation");
    if (found == entity.extensions.end() || !found->is_object() ||
        found->value("version", 0) != 1 || found->size() != 3 ||
        !found->contains("source_boundary_authoring") ||
        !found->contains("operations") || !found->at("operations").is_array() ||
        found->at("operations").empty()) {
        throw std::invalid_argument("Boundary geometry derivation is invalid");
    }
    const auto decoded = decode_boundary_receipt_envelope(
        found->at("source_boundary_authoring"));
    if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
    const auto replay = replay_boundary_construction(*decoded.record);
    IdentifiedBoundary result{replay.boundary_id, entity.type, {}};
    for (const auto& edge : replay.edges) {
        result.segments.push_back({edge.segment_id, edge.start_vertex_id,
                                   edge.end_vertex_id, edge.segment});
    }
    for (const auto& operation : found->at("operations")) {
        if (!operation.is_object() || operation.size() != 2 ||
            !operation.contains("kind") || !operation.at("kind").is_string() ||
            !operation.contains("value")) {
            throw std::invalid_argument("Boundary geometry derivation operation is invalid");
        }
        const auto kind = operation.at("kind").get<std::string>();
        if (kind == "geometry_edit") {
            result = apply_geometry_edit(
                result, decode_boundary_geometry_edit(operation.at("value")));
        } else if (kind == "vertex_batch") {
            if (!operation.at("value").is_array())
                throw std::invalid_argument("Boundary vertex batch must be an array");
            std::vector<BoundaryGeometryEdit> edits;
            for (const auto& edit : operation.at("value"))
                edits.push_back(decode_boundary_geometry_edit(edit));
            result = apply_vertex_batch(result, edits);
        } else if (kind == "transform") {
            result = apply_geometry_transform(
                result, decode_boundary_transform(operation.at("value")));
        } else {
            throw std::invalid_argument("Boundary geometry derivation operation kind is unsupported");
        }
    }
    return result;
}
} // namespace

void record_boundary_identities(BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& entities) {
    for (const auto& [id, entity] : entities) {
        const bool boundary = can_recognize_boundary_entity_type(entity.type);
        if (entity.type != "dimension" && !boundary) continue;
        auto [entry, inserted] = history.try_emplace(id);
        if (inserted || entry->second.entity_type.empty()) entry->second.entity_type = entity.type;
        if (boundary && !entity.properties.contains("boundary_model_version")) {
            entry->second.seen_legacy = true;
            continue;
        }
        if (!entry->second.protected_identity) entry->second.entity_type = entity.type;
        entry->second.protected_identity = true;
        if (!identified_v1(entity)) continue;
        for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
            entry->second.segments.insert(edge.segment_id);
            entry->second.vertices.insert(edge.start_vertex_id);
            entry->second.vertices.insert(edge.end_vertex_id);
        }
    }
}

void record_boundary_identity_transition(BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after) {
    for (const auto& [id, entity] : after) {
        const auto recorded = history.find(id);
        const bool seen_legacy = recorded != history.end() && recorded->second.seen_legacy;
        const bool legacy = can_recognize_boundary_entity_type(entity.type) &&
                            !entity.properties.contains("boundary_model_version");
        if (!seen_legacy && !legacy) continue;
        const auto previous = before.find(id);
        // Legacy-only histories retain their old permissive editing behavior.
        // Ambiguous identity reuse instead prevents a later promotion under
        // that same ID. Deletion alone and exact navigation do not taint it.
        if ((seen_legacy && previous == before.end()) ||
            (previous != before.end() && previous->second.type != entity.type))
            history[id].legacy_reused_or_retyped = true;
    }
    record_boundary_identities(history, after);
}

void validate_boundary_identity_transition(const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after) {
    for (const auto& [id, entity] : after) {
        const auto reserved = history.find(id);
        if (reserved == history.end()) continue;
        const auto previous = before.find(id);
        const auto invalid = [&](const char* reason) {
            throw std::invalid_argument("Boundary identity " + id + ": " + reason);
        };
        if (!reserved->second.protected_identity) {
            const bool becoming_protected = entity.type == "dimension" ||
                (can_recognize_boundary_entity_type(entity.type) &&
                 entity.properties.contains("boundary_model_version"));
            if (!becoming_protected) continue;
            // First identification must continue an unambiguous surviving
            // legacy identity. This also applies to opaque future versions:
            // parent lineage is known even when child semantics are unknown.
            if (reserved->second.legacy_reused_or_retyped || previous == before.end() ||
                previous->second.type != reserved->second.entity_type ||
                entity.type != reserved->second.entity_type ||
                !can_recognize_boundary_entity_type(previous->second.type) ||
                previous->second.properties.contains("boundary_model_version"))
                invalid("reused or retyped legacy boundary requires a fresh entity ID before identification");
            continue;
        }
        if (previous == before.end()) invalid("retired entity ID requires exact undo/redo");
        // Exact undo can restore pre-upgrade legacy state. Carrying it through
        // an unrelated branch is legal, but editing it requires a fresh upgrade.
        if (exact_entity(previous->second, entity)) continue;
        if (entity.type != reserved->second.entity_type ||
            (entity.type != "dimension" && !entity.properties.contains("boundary_model_version")))
            invalid("ordinary edit cannot strip registered semantics");
        if (!identified_v1(entity)) continue;

        std::map<std::string, std::pair<std::string, std::string>, std::less<>> active_edges;
        std::set<std::string, std::less<>> active_vertices;
        if (identified_v1(previous->second)) {
            for (const auto& edge : decode_identified_boundary_entity(previous->second).segments) {
                active_edges.emplace(edge.segment_id,
                    std::pair{edge.start_vertex_id, edge.end_vertex_id});
                active_vertices.insert(edge.start_vertex_id);
                active_vertices.insert(edge.end_vertex_id);
            }
        }
        bool reversal_checked = false;
        bool exact_reversal = false;
        for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
            const auto active = active_edges.find(edge.segment_id);
            if (reserved->second.segments.contains(edge.segment_id) && active == active_edges.end())
                invalid("retired segment ID requires exact undo/redo");
            for (const auto* vertex : {&edge.start_vertex_id, &edge.end_vertex_id})
                if (reserved->second.vertices.contains(*vertex) && !active_vertices.contains(*vertex))
                    invalid("retired vertex ID requires exact undo/redo");
            if (active == active_edges.end() ||
                active->second == std::pair{edge.start_vertex_id, edge.end_vertex_id}) continue;
            // Ordered pairs also protect two-edge lenses, whose edges share
            // the same unordered endpoint pair. Only a full typed reversal is
            // allowed to reverse surviving edge identities.
            if (!reversal_checked) {
                reversal_checked = true;
                try { exact_reversal = exact_entity(reverse_identified_boundary_entity(previous->second), entity); }
                catch (const std::invalid_argument&) { exact_reversal = false; }
            }
            if (!exact_reversal) invalid("surviving segment ID changed endpoint ownership");
        }
    }
}

std::map<std::string, Entity, std::less<>> translated_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTranslation& translation) {
    const auto found = source.find(translation.boundary_id);
    if (found == source.end()) throw std::invalid_argument("Translation boundary does not exist");
    const auto& original = found->second;
    auto boundary = decode_identified_boundary_entity(original);
    const bool derived = original.extensions.contains("boundary_geometry_derivation");
    if (!original.properties.contains("boundary_authoring") && !derived)
        throw std::invalid_argument(
            "Explicit boundary translation requires construction or geometry-derivation evidence");
    if (const auto unsupported = validate_boundary_integrity(source))
        throw std::invalid_argument(*unsupported);
    if (translation.offset.x == 0.0 && translation.offset.y == 0.0) return source;
    auto metadata = original;
    Entity encoded;
    if (derived) {
        const BoundaryTransformation transformation{
            translation.boundary_id,
            PlanarTransform{{}, 0.0, false, false, translation.offset}};
        boundary = apply_geometry_transform(boundary, transformation);
        metadata.extensions.at("boundary_geometry_derivation").at("operations")
            .push_back(geometry_transform_operation(transformation));
        encoded = encode_identified_boundary_entity(boundary, &metadata);
    } else {
        const auto decoded = decode_boundary_receipt_envelope(
            original.properties.at("boundary_authoring"));
        if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
        // Keep historical v1/v2 translation proofs byte-replayable. Framed
        // records retain local inputs and compose a world-space offset instead.
        const auto translated = decoded.record->schema_version == boundary_receipt_schema_version_v3
            ? transformed_boundary_construction(
                *decoded.record, PlanarTransform{{},0,false,false,translation.offset})
            : translated_boundary_construction(*decoded.record, translation.offset);
        const auto replay = replay_boundary_construction(translated);
        boundary.segments.clear();
        for (const auto& edge : replay.edges)
            boundary.segments.push_back(
                {edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, edge.segment});
        metadata.properties.erase("boundary_authoring");
        encoded = encode_identified_boundary_entity(boundary, &metadata);
        encoded.properties["boundary_authoring"] = encode_boundary_receipt_envelope(translated);
    }
    auto result = source;
    result.at(translation.boundary_id) = std::move(encoded);
    for (auto& [id, entity] : result) {
        (void)id;
        if (entity.type != "dimension" || !entity.properties.contains("target") ||
            entity.properties.at("target").value("entity_id", std::string{}) != translation.boundary_id) continue;
        const auto dimension = decode_boundary_dimension_entity(entity);
        if (!dimension.supported()) throw std::invalid_argument(dimension.unsupported_reason);
        auto moved = *dimension.dimension;
        moved.text_position.x += translation.offset.x;
        moved.text_position.y += translation.offset.y;
        entity = encode_boundary_dimension_entity(moved, &entity);
    }
    return result;
}

std::map<std::string, Entity, std::less<>> transformed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTransformation& transformation) {
    validate_boundary_transform(transformation);
    const auto found = source.find(transformation.boundary_id);
    if (found == source.end()) throw std::invalid_argument("Transform boundary does not exist");
    const auto& original = found->second;
    auto boundary = decode_identified_boundary_entity(original);
    const bool derived = original.extensions.contains("boundary_geometry_derivation");
    if (!original.properties.contains("boundary_authoring") && !derived)
        throw std::invalid_argument(
            "Explicit boundary transform requires construction or geometry-derivation evidence");
    if (const auto unsupported = validate_boundary_integrity(source))
        throw std::invalid_argument(*unsupported);
    const auto& transform = transformation.transform;
    if (transform.rotation_radians == 0 && !transform.flip_horizontal && !transform.flip_vertical &&
        transform.offset.x == 0 && transform.offset.y == 0) return source;
    auto metadata = original;
    Entity encoded;
    if (derived) {
        boundary = apply_geometry_transform(boundary, transformation);
        metadata.extensions.at("boundary_geometry_derivation").at("operations")
            .push_back(geometry_transform_operation(transformation));
        encoded = encode_identified_boundary_entity(boundary, &metadata);
    } else {
        const auto decoded = decode_boundary_receipt_envelope(
            original.properties.at("boundary_authoring"));
        if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
        const auto transformed = transformed_boundary_construction(*decoded.record, transform);
        const auto replay = replay_boundary_construction(transformed);
        boundary.segments.clear();
        for (const auto& edge : replay.edges)
            boundary.segments.push_back(
                {edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, edge.segment});
        metadata.properties.erase("boundary_authoring");
        encoded = encode_identified_boundary_entity(boundary, &metadata);
        encoded.properties["boundary_authoring"] = encode_boundary_receipt_envelope(transformed);
    }
    auto result = source;
    result.at(transformation.boundary_id) = std::move(encoded);
    for (auto& [id, entity] : result) {
        (void)id;
        if (entity.type != "dimension") continue;
        const auto dimension = decode_boundary_dimension_entity(entity);
        if (!dimension.supported()) throw std::invalid_argument(dimension.unsupported_reason);
        if (dimension.dimension->boundary_id != transformation.boundary_id) continue;
        auto moved = *dimension.dimension;
        moved.text_position = transform_point(moved.text_position, transform);
        entity = encode_boundary_dimension_entity(moved, &entity);
    }
    return result;
}

static std::map<std::string, Entity, std::less<>> edited_boundary_entities_impl(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit, const std::vector<BoundaryGeometryEdit>* batch) {
    validate_boundary_geometry_edit(edit);
    const auto found = source.find(edit.boundary_id);
    if (found == source.end()) throw std::invalid_argument("Edited boundary does not exist");
    const auto& original = found->second;
    const auto version = inspect_boundary_entity_version(original);
    if (version.format != BoundaryEntityFormat::identified_v1)
        throw std::invalid_argument("Boundary geometry editing requires a supported identified boundary");
    if (const auto unsupported = validate_boundary_integrity(source))
        throw std::invalid_argument(*unsupported);
    const auto edited = batch ? apply_vertex_batch(decode_identified_boundary_entity(original), *batch)
                              : apply_geometry_edit(decode_identified_boundary_entity(original), edit);
    if (edited == decode_identified_boundary_entity(original)) return source;

    auto metadata = original;
    const bool had_derivation = metadata.extensions.contains("boundary_geometry_derivation");
    if (metadata.properties.contains("boundary_authoring")) {
        if (had_derivation)
            throw std::invalid_argument("Boundary has conflicting geometry provenance");
        metadata.extensions["boundary_geometry_derivation"] = {
            {"version", 1},
            {"source_boundary_authoring", metadata.properties.at("boundary_authoring")},
            {"operations", nlohmann::json::array()}};
        metadata.properties.erase("boundary_authoring");
    }
    if (auto derivation = metadata.extensions.find("boundary_geometry_derivation");
        derivation != metadata.extensions.end()) {
        // Strictly replay before appending so malformed or stale evidence can
        // never be extended into an apparently valid history.
        if (had_derivation &&
            replay_geometry_derivation(original) != decode_identified_boundary_entity(original))
            throw std::invalid_argument("Boundary geometry derivation does not reproduce its source");
        if (batch) {
            auto values = nlohmann::json::array();
            for (const auto& item : *batch) values.push_back(encode_boundary_geometry_edit(item));
            derivation->at("operations").push_back({{"kind", "vertex_batch"}, {"value", values}});
        } else {
            derivation->at("operations").push_back(geometry_edit_operation(edit));
        }
    }
    auto encoded = encode_identified_boundary_entity(edited, &metadata);
    if (encoded.properties.contains("boundary")) {
        auto geometry = nlohmann::json::array();
        for (const auto& edge : edited.segments) {
            geometry.push_back({{"start", {edge.segment.start.x, edge.segment.start.y}},
                                {"end", {edge.segment.end.x, edge.segment.end.y}},
                                {"sweep_radians", edge.segment.sweep_radians}});
        }
        encoded.properties["boundary"] = std::move(geometry);
    }
    auto result = source;
    result.at(edit.boundary_id) = std::move(encoded);
    return result;
}

std::map<std::string, Entity, std::less<>> edited_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source, const BoundaryGeometryEdit& edit) {
    return edited_boundary_entities_impl(source, edit, nullptr);
}

std::map<std::string, Entity, std::less<>> edited_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits) {
    // Preserve already persisted format-8 proofs exactly whenever sequential
    // replay is valid. An invalid intermediate state is never published.
    try {
        auto sequential = source;
        for (const auto& edit : edits) sequential = edited_boundary_entities(sequential, edit);
        return sequential;
    } catch (const std::invalid_argument&) {
        std::map<std::string, std::vector<BoundaryGeometryEdit>, std::less<>> groups;
        for (const auto& edit : edits) groups[edit.boundary_id].push_back(edit);
        auto result = source;
        for (const auto& [id, group] : groups)
            result = edited_boundary_entities_impl(result, group.front(), &group);
        (void)validate_boundary_integrity(result);
        return result;
    }
}

std::optional<std::string> validate_boundary_integrity(
    const std::map<std::string, Entity, std::less<>>& entities) {
    std::optional<std::string> unsupported;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> edges;
    std::set<std::string, std::less<>> future_boundaries;
    for (const auto& [id, entity] : entities) {
        if (!can_recognize_boundary_entity_type(entity.type)) continue;
        const auto version = inspect_boundary_entity_version(entity);
        if (version.format == BoundaryEntityFormat::identified_v1) {
            const auto boundary = decode_identified_boundary_entity(entity);
            auto& index = edges[id];
            for (const auto& segment : boundary.segments) index.insert(segment.segment_id);
            // Qualify the owner first: same-named vendor metadata on legacy,
            // generic or unsupported-model entities has no receipt meaning.
            if (entity.properties.contains("boundary_authoring")) {
                const auto decoded = decode_boundary_receipt_envelope(
                    entity.properties.at("boundary_authoring"));
                if (!decoded.supported()) {
                    if (!unsupported) unsupported = "Boundary " + id + ": " + decoded.diagnostic;
                } else {
                    const auto replay = replay_boundary_construction(*decoded.record);
                    if (replay.boundary_id != boundary.id || replay.edges.size() != boundary.segments.size())
                        throw std::invalid_argument("Boundary " + id + ": construction owner or topology does not match");
                    for (std::size_t i = 0; i < replay.edges.size(); ++i) {
                        const auto& actual = boundary.segments[i];
                        const auto& expected = replay.edges[i];
                        if (!(actual == IdentifiedSegment{expected.segment_id, expected.start_vertex_id,
                                                         expected.end_vertex_id, expected.segment}))
                            throw std::invalid_argument("Boundary " + id +
                                ": canonical geometry or topology differs from construction input replay");
                    }
                }
            }
            if (entity.extensions.contains("boundary_geometry_derivation")) {
                if (entity.properties.contains("boundary_authoring"))
                    throw std::invalid_argument("Boundary " + id +
                        ": construction and geometry derivation evidence conflict");
                const auto replayed = replay_geometry_derivation(entity);
                if (replayed != boundary)
                    throw std::invalid_argument("Boundary " + id +
                        ": canonical geometry differs from geometry edit replay");
            }
        } else if (version.format == BoundaryEntityFormat::unsupported_version) {
            future_boundaries.insert(id);
            if (!unsupported) unsupported = "Boundary " + id + ": " + version.diagnostic;
        }
    }
    for (const auto& [id, entity] : entities) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) {
            if (!unsupported) unsupported = "Dimension " + id + ": " + decoded.unsupported_reason;
            continue;
        }
        const auto& dimension = *decoded.dimension;
        const auto owner = entities.find(dimension.boundary_id);
        if (owner == entities.end() || !can_recognize_boundary_entity_type(owner->second.type))
            throw std::invalid_argument("Dimension " + id + ": missing or invalid boundary owner");
        // The future geometry is opaque. Preserve a well-formed reference in
        // the read-only document without pretending to resolve its child IDs.
        if (future_boundaries.contains(owner->first)) continue;
        const auto boundary_edges = edges.find(owner->first);
        if (boundary_edges == edges.end())
            throw std::invalid_argument("Dimension " + id + ": missing identified source boundary");
        if (dimension.kind == BoundaryDimensionKind::segment_length) {
            if (!boundary_edges->second.contains(dimension.segment_id))
                throw std::invalid_argument("Dimension " + id + ": missing identified source segment");
        } else if (dimension.kind == BoundaryDimensionKind::angle) {
            if (!boundary_edges->second.contains(dimension.segment_id) ||
                !boundary_edges->second.contains(dimension.secondary_segment_id)) {
                throw std::invalid_argument("Dimension " + id + ": missing identified angle source segment");
            }
        }
    }
    return unsupported;
}

void validate_boundary_transition(const std::map<std::string, Entity, std::less<>>& before,
                                  const std::map<std::string, Entity, std::less<>>& after,
                                  bool allow_explicit_relationship_transform) {
    for (const auto& [id, entity] : before) {
        const auto found = after.find(id);
        if (found == after.end()) continue;
        const bool previous_receipt = identified_v1(entity) && entity.properties.contains("boundary_authoring");
        const bool next_receipt = identified_v1(found->second) &&
                                  found->second.properties.contains("boundary_authoring");
        if (previous_receipt != next_receipt)
            throw std::invalid_argument("Boundary " + id +
                ": surviving entities cannot acquire or lose construction receipts through a raw edit");
        if (previous_receipt &&
            (decode_identified_boundary_entity(entity) != decode_identified_boundary_entity(found->second) ||
             entity.properties.at("boundary_authoring").dump() !=
                 found->second.properties.at("boundary_authoring").dump()) &&
            !(allow_explicit_relationship_transform &&
              valid_explicit_relationship_transform(entity, found->second)))
            throw std::invalid_argument("Boundary " + id +
                ": construction-bound geometry, topology and inputs require an explicit derivation edit");
        const bool previous_derivation = identified_v1(entity) &&
            entity.extensions.contains("boundary_geometry_derivation");
        const bool next_derivation = identified_v1(found->second) &&
            found->second.extensions.contains("boundary_geometry_derivation");
        if (previous_derivation != next_derivation)
            throw std::invalid_argument("Boundary " + id +
                ": surviving entities cannot acquire or lose geometry derivation evidence through a raw edit");
        if (previous_derivation &&
            (decode_identified_boundary_entity(entity) !=
                 decode_identified_boundary_entity(found->second) ||
             entity.extensions.at("boundary_geometry_derivation").dump() !=
                 found->second.extensions.at("boundary_geometry_derivation").dump()))
            throw std::invalid_argument("Boundary " + id +
                ": derived geometry and its proof require an explicit typed command");
        if (entity.type == "dimension") {
            if (found->second.type != "dimension")
                throw std::invalid_argument("Dimension " + id + ": ordinary edit cannot strip dimension semantics");
        }
        if (!can_recognize_boundary_entity_type(entity.type)) continue;
        if (!entity.properties.contains("boundary_model_version")) {
            if (identified_v1(found->second)) {
                // Raw commands must obey the same collision and preservation
                // policy as the explicit upgrade helper. A marker flip must
                // never reinterpret vendor-opaque identity-shaped fields.
                LegacyBoundaryIdentityOptions identities;
                for (const auto& edge : decode_identified_boundary_entity(found->second).segments) {
                    identities.segment_ids.push_back(edge.segment_id);
                    identities.vertex_ids.push_back(edge.start_vertex_id);
                }
                const auto upgraded = upgrade_legacy_boundary_entity(entity, identities);
                if (!exact_entity(upgraded, found->second))
                    throw std::invalid_argument("Boundary " + id +
                        ": identity upgrade must preserve geometry, type and opaque metadata");
            }
            continue;
        }
        if (found->second.type != entity.type ||
            !found->second.properties.contains("boundary_model_version"))
            throw std::invalid_argument("Boundary " + id + ": ordinary edit cannot strip identified boundary semantics");
    }
}
} // namespace sketch
