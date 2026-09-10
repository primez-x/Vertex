#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_receipt.hpp"
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
        if (boundary_edges == edges.end() || !boundary_edges->second.contains(dimension.segment_id))
            throw std::invalid_argument("Dimension " + id + ": missing identified source segment");
    }
    return unsupported;
}

void validate_boundary_transition(const std::map<std::string, Entity, std::less<>>& before,
                                  const std::map<std::string, Entity, std::less<>>& after) {
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
                 found->second.properties.at("boundary_authoring").dump()))
            throw std::invalid_argument("Boundary " + id +
                ": construction-bound geometry, topology and inputs require an explicit derivation edit");
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
