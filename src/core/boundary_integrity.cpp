#include "sketch/boundary_integrity.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/wall_merge.hpp"
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
#include "sketch/physical_wall_room_merge.hpp"
#endif
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/model_phases.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <numbers>
#include <set>
#include <string_view>

namespace sketch {
namespace {
bool exact_entity(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
           left.extensions.dump() == right.extensions.dump();
}
// A reviewed automatic set uses one explicitly homogeneous template. Only
// the analytical target and actual text position may differ across its edges.
nlohmann::json automatic_dimension_template_metadata(const Entity& entity) {
    auto properties=entity.properties;
    properties.erase("text_position");
    if (properties.contains("target"))
        for (const auto* key:{"entity_id","segment_id","segment_ids","second_segment_id","vertex_id"})
            properties.at("target").erase(key);
    return {{"type",entity.type},{"required",entity.required},
        {"properties",std::move(properties)},{"extensions",entity.extensions}};
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
        if ((new_record.schema_version != boundary_receipt_schema_version_v3 && new_record.schema_version != boundary_receipt_schema_version_v4) ||
            new_record.transforms.empty() ||
            !same_record_without_transforms(old_record, new_record)) {
            return false;
        }
        if (old_record.schema_version == boundary_receipt_schema_version_v3 || old_record.schema_version == boundary_receipt_schema_version_v4) {
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
    if (edit.kind == BoundaryGeometryEditKind::insert_vertex)
        return insert_boundary_vertex(source, edit.target_id, edit.fraction,
                                      edit.new_vertex_id, edit.new_segment_id);
    if (edit.kind == BoundaryGeometryEditKind::reconstruct_arc)
        return reconstruct_boundary_arc(source, edit.target_id, *edit.arc_construction);
    if (edit.kind == BoundaryGeometryEditKind::redefine_boundary) {
        const std::set<std::string> fields{"segment_id", "start_vertex_id", "end_vertex_id", "start", "end", "sweep_radians"};
        for (const auto& edge : edit.replacement_segments) {
            if (!edge.is_object()) throw std::invalid_argument("Redefinition edge must be an object");
            std::set<std::string> actual;
            for (const auto& [key, ignored] : edge.items()) { (void)ignored; actual.insert(key); }
            if (actual != fields) throw std::invalid_argument("Redefinition edge contains unsupported fields");
        }
        const auto replacement = decode_identified_boundary_entity(Entity{source.id, source.type,
            {{"boundary_model_version", 1}, {"segments", edit.replacement_segments}}, false, nlohmann::json::object()});
        if (!edit.fresh_topology && replacement.segments.size() == source.segments.size()) {
            if (!edit.replacement_child_mapping.empty() || !edit.replacement_removed_reference_ids.empty() || edit.allow_automatic_angle_removal)
                throw std::invalid_argument("Reference decisions require changed boundary topology");
            for (std::size_t i = 0; i < source.segments.size(); ++i)
                if (replacement.segments[i].segment_id != source.segments[i].segment_id ||
                    replacement.segments[i].start_vertex_id != source.segments[i].start_vertex_id ||
                    replacement.segments[i].end_vertex_id != source.segments[i].end_vertex_id)
                    throw std::invalid_argument("Same-count redefinition must retain ordered child identities");
        } else {
            std::set<std::string> retired;
            for (const auto& edge : source.segments) {
                retired.insert(edge.segment_id); retired.insert(edge.start_vertex_id); retired.insert(edge.end_vertex_id);
            }
            for (const auto& edge : replacement.segments)
                if (retired.contains(edge.segment_id) || retired.contains(edge.start_vertex_id) || retired.contains(edge.end_vertex_id))
                    throw std::invalid_argument("Fresh-topology redefinition requires entirely new child identities");
            std::set<std::string> old_segments, old_vertices, new_segments, new_vertices;
            for (const auto& edge : source.segments) {
                old_segments.insert(edge.segment_id);
                old_vertices.insert(edge.start_vertex_id); old_vertices.insert(edge.end_vertex_id);
            }
            for (const auto& edge : replacement.segments) {
                new_segments.insert(edge.segment_id);
                new_vertices.insert(edge.start_vertex_id); new_vertices.insert(edge.end_vertex_id);
            }
            if (!edit.replacement_child_mapping.empty()) {
                for (const auto* group : {"segments", "vertices"}) {
                    const bool segment = std::string_view(group) == "segments";
                    const auto& old_children = segment ? old_segments : old_vertices;
                    const auto& new_children = segment ? new_segments : new_vertices;
                    std::set<std::string> destinations;
                    for (const auto& [old_id, target] : edit.replacement_child_mapping.at(group).items()) {
                        const auto& new_id = target.get_ref<const std::string&>();
                        if (!old_children.contains(old_id) || !new_children.contains(new_id))
                            throw std::invalid_argument("Reference mapping must connect existing old and new children of the same kind");
                        if (!destinations.insert(new_id).second)
                            throw std::invalid_argument("Reference mapping cannot merge distinct old children in one namespace");
                    }
                }
            }
        }
        if (!edit.replacement_authoring.is_null()) {
            const auto decoded = decode_boundary_receipt_envelope(edit.replacement_authoring);
            if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
            const auto replay = replay_boundary_construction(*decoded.record);
            if (replay.edges.size() != replacement.segments.size())
                throw std::invalid_argument("Redefinition construction topology does not match");
            for (std::size_t i = 0; i < replay.edges.size(); ++i)
                if (!(IdentifiedSegment{replacement.segments[i].segment_id,
                    replacement.segments[i].start_vertex_id, replacement.segments[i].end_vertex_id,
                    replay.edges[i].segment} == replacement.segments[i]))
                    throw std::invalid_argument("Redefinition geometry differs from new construction replay");
        }
        return replacement;
    }
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

Vec2 split_dimension_position(const Segment& segment, double side) {
    const auto dx = segment.end.x - segment.start.x;
    const auto dy = segment.end.y - segment.start.y;
    Vec2 midpoint{std::midpoint(segment.start.x, segment.end.x),
                  std::midpoint(segment.start.y, segment.end.y)};
    auto tangent = std::atan2(dy, dx);
    if (segment.sweep_radians != 0.0) {
        const auto k = 0.5 / std::tan(segment.sweep_radians / 2.0);
        const Vec2 center{midpoint.x - dy * k, midpoint.y + dx * k};
        const auto half = segment.sweep_radians * 0.5;
        const auto x = segment.start.x - center.x;
        const auto y = segment.start.y - center.y;
        midpoint = {center.x + x * std::cos(half) - y * std::sin(half),
                    center.y + x * std::sin(half) + y * std::cos(half)};
        tangent = std::atan2(midpoint.y - center.y, midpoint.x - center.x) +
            (segment.sweep_radians > 0 ? std::numbers::pi / 2 : -std::numbers::pi / 2);
    }
    const auto offset = std::max(0.25, segment_length(segment) * 0.1);
    return {midpoint.x - std::sin(tangent) * offset * side,
            midpoint.y + std::cos(tangent) * offset * side};
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
        !found->contains("version") || !found->at("version").is_number_integer() ||
        (found->value("version", 0) != 1 && found->value("version", 0) != 2) || found->size() != 3 ||
        !found->contains("operations") || !found->at("operations").is_array() ||
        found->at("operations").empty()) {
        throw std::invalid_argument("Boundary geometry derivation is invalid");
    }
    IdentifiedBoundary result;
    if (found->at("version") == 1) {
        if (!found->contains("source_boundary_authoring"))
            throw std::invalid_argument("Boundary geometry derivation requires its construction origin");
        const auto decoded = decode_boundary_receipt_envelope(found->at("source_boundary_authoring"));
        if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
        const auto replay = replay_boundary_construction(*decoded.record);
        result = {replay.boundary_id, entity.type, {}};
        for (const auto& edge : replay.edges)
            result.segments.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, edge.segment});
    } else {
        if (!found->contains("source_boundary") || !found->at("source_boundary").is_object() ||
            found->at("source_boundary").size() != 2 ||
            !found->at("source_boundary").contains("boundary_model_version") ||
            !found->at("source_boundary").contains("segments"))
            throw std::invalid_argument("Boundary geometry derivation requires a strict identified topology origin");
        result = decode_identified_boundary_entity(Entity{entity.id, entity.type,
            found->at("source_boundary"), false, nlohmann::json::object()});
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
        } else if (kind == "physical_room_wall_merge") {
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
            result = replay_physical_room_wall_merge(entity, result, operation.at("value"));
#else
            throw std::invalid_argument("Physical room wall merging requires the architectural geometry engine");
#endif
        } else if (kind == "wall_merge") {
            const auto& value=operation.at("value");
            if(!value.is_object() || value.size()!=5 || !value.contains("version") ||
                !value.at("version").is_number_integer() || value.at("version")!=1 ||
                !value.contains("vertex_id") || !value.at("vertex_id").is_string() ||
                !value.contains("segments") || !value.at("segments").is_array() ||
                !value.contains("wall_source_ids") || !value.at("wall_source_ids").is_array() ||
                !value.contains("removed_wall_id") || !value.at("removed_wall_id").is_string())
                throw std::invalid_argument("Wall merge boundary derivation has unsupported fields");
            const auto vertex=value.at("vertex_id").get<std::string>();
            const auto outgoing=std::find_if(result.segments.begin(),result.segments.end(),
                [&](const auto& edge){return edge.start_vertex_id==vertex;});
            if(outgoing==result.segments.end())throw std::invalid_argument("Wall merge boundary seam is unavailable");
            const auto index=static_cast<std::size_t>(outgoing-result.segments.begin());
            const auto& incoming=result.segments[(index+result.segments.size()-1)%result.segments.size()];
            const auto expected=remove_boundary_vertex(result,vertex);
            const auto merged=std::find_if(expected.segments.begin(),expected.segments.end(),
                [&](const auto& edge){return edge.segment_id==incoming.segment_id;});
            if((incoming.segment.sweep_radians==0)!=(outgoing->segment.sweep_radians==0) ||
                std::abs(segment_length(merged->segment)-segment_length(incoming.segment)-segment_length(outgoing->segment))>
                    default_geometry_tolerance_metres ||
                std::abs(merged->segment.sweep_radians-incoming.segment.sweep_radians-outgoing->segment.sweep_radians)>1e-9)
                throw std::invalid_argument("Wall merge boundary derivation does not preserve its analytical span");
            if(incoming.segment.sweep_radians==0) {
                const auto& support=merged->segment;
                const auto dx=support.end.x-support.start.x,dy=support.end.y-support.start.y,length=std::hypot(dx,dy);
                const auto deviation=std::abs((incoming.segment.end.x-support.start.x)*(dy/length)-
                    (incoming.segment.end.y-support.start.y)*(dx/length));
                if(!std::isfinite(deviation) || deviation>default_geometry_tolerance_metres)
                    throw std::invalid_argument("Wall merge boundary seam leaves its straight support line");
            }
            static const std::set<std::string> fields={"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"};
            for(const auto& edge:value.at("segments")) {
                if(!edge.is_object())throw std::invalid_argument("Wall merge boundary segment must be an object");
                std::set<std::string> actual;
                for(const auto& [key,ignored]:edge.items()){(void)ignored;actual.insert(key);}
                if(actual!=fields)throw std::invalid_argument("Wall merge boundary segment contains unsupported fields");
            }
            const auto replacement=decode_identified_boundary_entity(Entity{entity.id,entity.type,
                {{"boundary_model_version",1},{"segments",value.at("segments")}},false,nlohmann::json::object()});
            if(replacement.segments.size()!=expected.segments.size())
                throw std::invalid_argument("Wall merge boundary derivation changed unrelated topology");
            for(std::size_t i=0;i<replacement.segments.size();++i) {
                const auto& a=expected.segments[i];const auto& b=replacement.segments[i];
                const auto bounds=segment_bounds(b.segment);
                const auto support_point=[](const Segment& segment,double fraction) {
                    if(segment.sweep_radians==0)return Vec2{std::lerp(segment.start.x,segment.end.x,fraction),
                        std::lerp(segment.start.y,segment.end.y,fraction)};
                    const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y,chord=std::hypot(dx,dy);
                    const auto angle=segment.sweep_radians*fraction,half_sine=std::sin(angle/2);
                    const auto center_offset=std::abs(segment.sweep_radians)==std::numbers::pi?0.0:chord/(2*std::tan(segment.sweep_radians/2));
                    const auto along=chord*half_sine*half_sine+center_offset*std::sin(angle);
                    const auto normal=-chord*std::sin(angle)/2+center_offset*(2*half_sine*half_sine);
                    return Vec2{std::fma(dx/chord,along,std::fma(-dy/chord,normal,segment.start.x)),
                        std::fma(dy/chord,along,std::fma(dx/chord,normal,segment.start.y))};
                };
                bool same_support=true;
                for(const auto fraction:{0.25,0.5,0.75}) {
                    const auto first=support_point(a.segment,fraction),second=support_point(b.segment,fraction);
                    const auto displacement=std::hypot(first.x-second.x,first.y-second.y);
                    same_support=same_support&&std::isfinite(displacement)&&displacement<=default_geometry_tolerance_metres;
                }
                if(a.segment_id!=b.segment_id || a.start_vertex_id!=b.start_vertex_id || a.end_vertex_id!=b.end_vertex_id ||
                    std::hypot(a.segment.start.x-b.segment.start.x,a.segment.start.y-b.segment.start.y)>default_geometry_tolerance_metres ||
                    std::hypot(a.segment.end.x-b.segment.end.x,a.segment.end.y-b.segment.end.y)>default_geometry_tolerance_metres ||
                    (a.segment.sweep_radians==0)!=(b.segment.sweep_radians==0) ||
                    (a.segment.sweep_radians!=0 && std::signbit(a.segment.sweep_radians)!=std::signbit(b.segment.sweep_radians)) ||
                    std::max(segment_length(a.segment),segment_length(b.segment))*
                        std::abs(a.segment.sweep_radians-b.segment.sweep_radians)>default_geometry_tolerance_metres || !same_support ||
                    !std::isfinite(bounds.minimum.x) || !std::isfinite(bounds.minimum.y) ||
                    !std::isfinite(bounds.maximum.x) || !std::isfinite(bounds.maximum.y) ||
                    std::max({std::abs(bounds.minimum.x),std::abs(bounds.minimum.y),std::abs(bounds.maximum.x),std::abs(bounds.maximum.y)})>1e6)
                    throw std::invalid_argument("Wall merge boundary derivation lost surviving correspondence or changed geometry");
            }
            // Reuse the strict source-list/identifier contract; geometry is
            // already proved above and cannot be supplied as edit authority.
            BoundaryGeometryEdit proof;proof.boundary_id=proof.target_id=entity.id;
            proof.kind=BoundaryGeometryEditKind::redefine_boundary;proof.replacement_segments=value.at("segments");
            proof.replacement_wall_source_ids=value.at("wall_source_ids").get<std::vector<std::string>>();
            validate_boundary_geometry_edit(proof);
            const auto removed_id=value.at("removed_wall_id").get<std::string>();
            if(removed_id.empty() || removed_id.size()>128 || !std::all_of(removed_id.begin(),removed_id.end(),[](unsigned char c) {
                return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c==':';
            }))throw std::invalid_argument("Wall merge boundary retired source identity is invalid");
            if(proof.replacement_wall_source_ids.empty() ||
                std::find(proof.replacement_wall_source_ids.begin(),proof.replacement_wall_source_ids.end(),
                    removed_id)!=proof.replacement_wall_source_ids.end())
                throw std::invalid_argument("Wall merge boundary source still includes its retired wall");
            result=replacement;
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
        if (entity.type == "wall" && entity.extensions.contains("wall_merge_archive")) {
            const auto& archive = entity.extensions.at("wall_merge_archive");
            if (archive.is_object() && archive.contains("version") && archive.at("version") == 1) {
                validate_wall_merge_archive(entity);
                std::vector<const nlohmann::json*> pending{&archive};
                while (!pending.empty()) {
                    const auto* retained = pending.back();
                    pending.pop_back();
                    const auto& sources = retained->at("sources");
                    history[sources.at(1).at("id").get<std::string>()].wall_merge_reserved = true;
                    for (const auto& source : sources) {
                        const auto& extensions = source.at("extensions");
                        if (extensions.contains("wall_merge_archive"))
                            pending.push_back(&extensions.at("wall_merge_archive"));
                    }
                }
            }
        }
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
    const std::map<std::string, Entity, std::less<>>& after,
    const BoundaryGeometryEdit* typed_edit) {
    // A split exception is scoped to a typed intent and its entire canonical
    // entity state, including dimension and constraint migrations. Raw edits
    // cannot authorize endpoint rebinding by presenting equivalent geometry.
    bool verified_split = false;
    if (typed_edit && typed_edit->kind == BoundaryGeometryEditKind::insert_vertex) {
        const auto expected = edited_boundary_entities(before, *typed_edit);
        verified_split = expected.size() == after.size();
        if (verified_split) {
            for (const auto& [id, entity] : expected) {
                const auto actual = after.find(id);
                if (actual == after.end() || !exact_entity(entity, actual->second)) {
                    verified_split = false;
                    break;
                }
            }
        }
        if (!verified_split)
            throw std::invalid_argument("Boundary split state differs from typed reconstruction");
    }
    for (const auto& [id, entity] : after) {
        const auto reserved = history.find(id);
        if (reserved == history.end()) continue;
        const auto previous = before.find(id);
        const auto invalid = [&](const char* reason) {
            throw std::invalid_argument("Boundary identity " + id + ": " + reason);
        };
        if (reserved->second.wall_merge_reserved && previous == before.end())
            invalid("merged wall ID requires exact undo/redo; use a fresh identity for a new object");
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
            if (verified_split && id == typed_edit->boundary_id &&
                edge.segment_id == typed_edit->target_id &&
                edge.start_vertex_id == active->second.first &&
                edge.end_vertex_id == typed_edit->new_vertex_id) continue;
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
        const auto translated = (decoded.record->schema_version == boundary_receipt_schema_version_v3 || decoded.record->schema_version == boundary_receipt_schema_version_v4)
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

static std::map<std::string, Entity, std::less<>> transformed_boundary_entities_impl(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTransformation& transformation, bool permit_plain_origin, bool validate_source) {
    validate_boundary_transform(transformation);
    const auto found = source.find(transformation.boundary_id);
    if (found == source.end())
        throw std::invalid_argument("Transform boundary does not exist");
    const auto& original = found->second;
    auto boundary = decode_identified_boundary_entity(original);
    const bool derived = original.extensions.contains("boundary_geometry_derivation");
    if (!original.properties.contains("boundary_authoring") && !derived && !permit_plain_origin)
        throw std::invalid_argument(
            "Explicit boundary transform requires construction or geometry-derivation evidence");
    if (validate_source) {
        if (const auto unsupported = validate_boundary_integrity(source))
            throw std::invalid_argument(*unsupported);
    }
    const auto& transform = transformation.transform;
    if (transform.rotation_radians == 0 && !transform.flip_horizontal && !transform.flip_vertical &&
        transform.offset.x == 0 && transform.offset.y == 0)
        return source;
    auto metadata = original;
    Entity encoded;
    if (derived || !original.properties.contains("boundary_authoring")) {
        if (!derived) {
            metadata.extensions["boundary_geometry_derivation"] = {
                {"version", 2}, {"source_boundary", {{"boundary_model_version", 1},
                    {"segments", original.properties.at("segments")}}}, {"operations", nlohmann::json::array()}};
        }
        boundary = apply_geometry_transform(boundary, transformation);
        metadata.extensions.at("boundary_geometry_derivation").at("operations")
            .push_back(geometry_transform_operation(transformation));
        encoded = encode_identified_boundary_entity(boundary, &metadata);
    } else {
        const auto decoded = decode_boundary_receipt_envelope(
            original.properties.at("boundary_authoring"));
        if (!decoded.supported())
            throw std::invalid_argument(decoded.diagnostic);
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
        if (entity.type != "dimension")
            continue;
        const auto dimension = decode_boundary_dimension_entity(entity);
        if (!dimension.supported())
            throw std::invalid_argument(dimension.unsupported_reason);
        if (dimension.dimension->boundary_id != transformation.boundary_id)
            continue;
        auto moved = *dimension.dimension;
        moved.text_position = transform_point(moved.text_position, transform);
        entity = encode_boundary_dimension_entity(moved, &entity);
    }
    return result;
}

std::map<std::string, Entity, std::less<>> transformed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTransformation& transformation) {
    return transformed_boundary_entities_impl(source, transformation, false, true);
}

std::map<std::string, Entity, std::less<>> transformed_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryTransformation>& transformations) {
    if (transformations.empty())
        throw std::invalid_argument("Boundary transform group is empty");
    if (const auto unsupported = validate_boundary_integrity(source))
        throw std::invalid_argument(*unsupported);
    std::set<std::string> owners;
    const auto& shared = transformations.front().transform;
    for (const auto& transformation : transformations) {
        validate_boundary_transform(transformation);
        if (!owners.insert(transformation.boundary_id).second || !(transformation.transform == shared))
            throw std::invalid_argument("Boundary transform group requires unique owners and one shared transform");
    }
    auto result = source;
    for (const auto& transformation : transformations) {
        const auto transformed = transformed_boundary_entities_impl(source, transformation, true, false);
        result.at(transformation.boundary_id) = transformed.at(transformation.boundary_id);
        for (const auto& [id, entity] : source) {
            if (entity.type != "dimension")
                continue;
            const auto dimension = decode_boundary_dimension_entity(entity);
            if (dimension.dimension && dimension.dimension->boundary_id == transformation.boundary_id)
                result.at(id) = transformed.at(id);
        }
    }
    return result;
}

static bool exact_replacement_outline(const Boundary& requested, const Boundary& derived) {
    if (requested.size() != derived.size()) return false;
    const auto equal = [](const Segment& a, const Segment& b) {
        return a.start.x == b.start.x && a.start.y == b.start.y &&
            a.end.x == b.end.x && a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
    };
    for (std::size_t offset = 0; offset < derived.size(); ++offset) {
        bool forward = true, reverse = true;
        for (std::size_t i = 0; i < requested.size(); ++i) {
            forward = forward && equal(requested[i], derived[(offset + i) % derived.size()]);
            const auto& backwards = derived[(offset + derived.size() - i) % derived.size()];
            reverse = reverse && equal(requested[i], Segment{backwards.end, backwards.start, -backwards.sweep_radians});
        }
        if (forward || reverse) return true;
    }
    return false;
}

static void validate_retained_replacement_deductions(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const Boundary& replacement) {
    const auto deductions = owner.properties.find("deduction_ids");
    if (deductions == owner.properties.end()) return;
    if (!deductions->is_array()) throw std::invalid_argument("Measured area deductions must be an array");
    const auto organization = organize_project(entities);
    const auto context = organization.drawing_context(owner.id);
    std::set<std::string> unique;
    for (const auto& value : *deductions) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            !unique.insert(value.get<std::string>()).second)
            throw std::invalid_argument("Retained deduction identifiers are invalid");
        const auto found = entities.find(value.get<std::string>());
        if (found == entities.end() || found->first == owner.id ||
            (found->second.type != "boundary" && found->second.type != "measurement_boundary"))
            throw std::invalid_argument("Retained deduction is missing or is not a measured area");
        const auto child_context = organization.drawing_context(found->first);
        if (!context || !child_context || context->property_id != child_context->property_id ||
            context->building_id != child_context->building_id || context->floor_id != child_context->floor_id)
            throw std::invalid_argument("Retained deduction belongs to a different measurement context");
        const auto child = inspect_boundary_entity_version(found->second).format == BoundaryEntityFormat::identified_v1
            ? found->second : upgrade_legacy_boundary_entity(found->second);
        // Validate each deduction independently: overlapping deductions remain
        // legitimate calculation tools, while none may escape the new parent.
        if (const auto diagnostic = validate_boundary_holes(replacement,
            {boundary_geometry(decode_identified_boundary_entity(child))}))
            throw std::invalid_argument("Retained deduction does not fit replacement measured area: " + *diagnostic);
    }
}

static void validate_replacement_linework_face(
    const std::map<std::string, Entity, std::less<>>& source, const Entity& original,
    const IdentifiedBoundary& replacement, const BoundaryGeometryEdit& edit) {
    if(original.type=="measurement_boundary" && original.extensions.contains("measurement_linework_group"))
        throw std::invalid_argument("Combined measured areas require a complete group refresh; single-face source replacement cannot change their membership.");
    if (original.type != "measurement_boundary" ||
        !original.extensions.contains("measurement_linework_sources") ||
        original.properties.contains("wall_measurement_source"))
        throw std::invalid_argument("Measured-line source replacement requires a retained measured-line area owner");
    const auto organization = organize_project(source);
    const auto context = organization.drawing_context(original.id);
    if (!context || !context->complete())
        throw std::invalid_argument("Measured-line source replacement requires a complete drawing context");
    // Phase membership is semantic. Drawing filters, ordinary layer hiding and
    // saved-view presentation must not reduce the graph used for source proof.
    std::set<std::string, std::less<>> available;
    for (const auto& [id, entity] : source) { (void)entity; available.insert(id); }
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type != "model_phases") continue;
        const auto phases = ModelPhases::from_json(entity.properties.at("model"));
        const auto active = phases.active_state();
        for (const auto& member : phases.entity_ids())
            if (!active.contains(member) || active.at(member) == ModelPhase::demolished) available.erase(member);
    }
    if (!available.contains(original.id))
        throw std::invalid_argument("Measured area is unavailable in the active design phase");
    auto metadata = original;
    metadata.extensions["measurement_linework_sources"] = *edit.replacement_linework_sources;
    auto candidate = source;
    // This temporary map proves only the current graph face. Receipt and
    // reference retirement remains the responsibility of the actual edit
    // below, which archives authoring and validates every dependent reference.
    metadata.properties["segments"] = encode_identified_boundary_entity(replacement).properties.at("segments");
    candidate.at(original.id) = std::move(metadata);
    const auto checks = measurement_linework_source_checks(candidate, &available);
    const auto found = checks.find(original.id);
    if (found == checks.end() || !found->second.current)
        throw std::invalid_argument("Replacement measured area is not an exact current source face: " +
            (found == checks.end() ? std::string("source lineage is missing") : found->second.diagnostic));
    const auto outline = boundary_geometry(replacement);
    for (const auto& [id, check] : checks)
        if (id != original.id && available.contains(id) && check.current && organization.drawing_context(id) == context &&
            exact_replacement_outline(outline, boundary_geometry(decode_identified_boundary_entity(candidate.at(id)))))
            throw std::invalid_argument("Replacement measured source face is already assigned to another area: " + id);
    validate_retained_replacement_deductions(source, original, outline);
    for (const auto& [key, value] : edit.replacement_properties.items())
        if (!original.properties.contains(key) || original.properties.at(key) != value)
            throw std::invalid_argument("Measured-line source replacement must retain the measured area's metadata");
}

static std::map<std::string, Entity, std::less<>> edited_boundary_entities_impl(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit, const std::vector<BoundaryGeometryEdit>* batch,
    bool retained_replay = false, const std::set<std::string>& reviewed_room_owners = {}) {
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
    std::optional<PhysicalWallRoomDescriptor> repaired_room;
    if (edit.physical_wall_room_repair) {
        if (batch) throw std::invalid_argument("Physical room repair cannot be batched with other geometry edits");
        repaired_room=validate_physical_wall_room_repair(source,edit,reviewed_room_owners);
        validate_retained_replacement_deductions(source,original,boundary_geometry(edited));
    }
    if (edit.replacement_linework_sources) {
        if (batch) throw std::invalid_argument("Measured-line source replacement cannot be a vertex batch");
        validate_replacement_linework_face(source, original, edited, edit);
    }
    std::optional<WallMeasurementResult> replacement_source;
    if (!edit.replacement_wall_source_ids.empty()) {
        if (batch) throw std::invalid_argument("Wall source replacement cannot be a vertex batch");
        const auto replacement_outline = boundary_geometry(edited);
        try {
            replacement_source = edit.wall_source_translation
                ? derive_translated_exterior_wall_measurement(source,original,edit.replacement_wall_source_ids,*edit.wall_source_translation)
                : derive_replacement_exterior_wall_measurement(source, original,edit.replacement_wall_source_ids);
        } catch (const std::invalid_argument&) {
            if (!retained_replay || edit.wall_source_translation) throw;
        }
        if (retained_replay && !edit.wall_source_translation && (!replacement_source ||
            !exact_replacement_outline(replacement_outline, replacement_source->boundary))) {
            // Archives do not identify the numerical kernel, and old commands
            // may survive in newer files. Accept only the complete exact old
            // result, with the same hierarchy/phase/elevation checks. Keep the
            // caller's analytical bytes and derivation proof untouched.
            replacement_source = derive_legacy_replacement_exterior_wall_measurement(source, original,
                edit.replacement_wall_source_ids);
        }
        if (!replacement_source || !exact_replacement_outline(replacement_outline, replacement_source->boundary))
            throw std::invalid_argument("Replacement measured outline differs from the supplied exterior source walls");
        validate_retained_replacement_deductions(source, original, replacement_outline);
        for (const auto& [key, value] : edit.replacement_properties.items())
            if (!original.properties.contains(key) || original.properties.at(key) != value)
                throw std::invalid_argument("Wall source replacement must retain the measured area's metadata");
    }
    if (edit.kind != BoundaryGeometryEditKind::redefine_boundary &&
        edited == decode_identified_boundary_entity(original)) return source;

    auto metadata = original;
    if (repaired_room) metadata.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor(*repaired_room);
    if (edit.replacement_linework_sources)
        metadata.extensions["measurement_linework_sources"] = *edit.replacement_linework_sources;
    if (replacement_source) metadata.properties["wall_measurement_source"] = replacement_source->source;
    if (edit.kind == BoundaryGeometryEditKind::redefine_boundary)
        for (const auto& [key, value] : edit.replacement_properties.items()) metadata.properties[key] = value;
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
    if ((replacement_source || edit.replacement_linework_sources || edit.fresh_topology || edit.allow_automatic_angle_removal) && !had_derivation &&
        !metadata.extensions.contains("boundary_geometry_derivation")) {
        metadata.extensions["boundary_geometry_derivation"] = {
            {"version", 2}, {"source_boundary", {
                {"boundary_model_version", 1}, {"segments", original.properties.at("segments")}}},
            {"operations", nlohmann::json::array()}};
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
    if (edit.kind == BoundaryGeometryEditKind::redefine_boundary &&
        !edit.replacement_authoring.is_null() &&
        !metadata.extensions.contains("boundary_geometry_derivation")) {
        auto record = *decode_boundary_receipt_envelope(edit.replacement_authoring).record;
        record.boundary_id = edited.id;
        for (std::size_t i = 0; i < record.edges.size(); ++i) {
            record.edges[i].segment_id = edited.segments[i].segment_id;
            record.edges[i].start_vertex_id = edited.segments[i].start_vertex_id;
            record.edges[i].end_vertex_id = edited.segments[i].end_vertex_id;
            record.edges[i].receipt.segment_id = edited.segments[i].segment_id;
        }
        encoded.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    }
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
    if (!batch && edit.kind == BoundaryGeometryEditKind::redefine_boundary) {
        const auto topology_changed = edit.fresh_topology || edited.segments.size() != decode_identified_boundary_entity(original).segments.size();
        const Entity* automatic_template = nullptr;
        std::vector<std::string> retired_dimensions;
        const std::set<std::string> removed_references(edit.replacement_removed_reference_ids.begin(),
            edit.replacement_removed_reference_ids.end());
        std::set<std::string> accepted_removals;
        bool removed_automatic_angle = false;
        std::set<std::pair<std::string, std::string>> used_mapping;
        const auto mapped_child = [&](const std::string& old_id, const char* group) {
            if (edit.replacement_child_mapping.empty())
                throw std::invalid_argument("Retained redraw reference requires an explicit child mapping: " + old_id);
            const auto& mapping = edit.replacement_child_mapping.at(group);
            const auto mapped = mapping.find(old_id);
            if (mapped == mapping.end())
                throw std::invalid_argument("Retained redraw reference requires an explicit child mapping: " + old_id);
            used_mapping.emplace(group, old_id);
            return mapped->get<std::string>();
        };
        for (auto& [id, entity] : result) {
            if (topology_changed && entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
                const auto& bindings = decoded.constraint->bindings;
                const bool affected = std::any_of(bindings.begin(), bindings.end(),
                    [&](const auto& binding) { return binding.owner_id == edit.boundary_id; });
                if (affected && removed_references.contains(id)) {
                    accepted_removals.insert(id);
                    continue;
                }
                for (std::size_t i = 0; i < bindings.size(); ++i) {
                    const auto& binding = bindings[i];
                    if (binding.owner_id != edit.boundary_id) continue;
                    const auto segment_id = mapped_child(binding.segment_id, "segments");
                    const auto vertex_id = mapped_child(binding.vertex_id, "vertices");
                    const auto target = std::find_if(edited.segments.begin(), edited.segments.end(),
                        [&](const auto& edge) { return edge.segment_id == segment_id; });
                    if (target == edited.segments.end() ||
                        (target->start_vertex_id != vertex_id && target->end_vertex_id != vertex_id))
                        throw std::invalid_argument("Mapped constraint vertex is not an endpoint of its mapped segment");
                    auto& persisted = entity.properties.at("bindings").at(i);
                    persisted.at("segment_id") = segment_id;
                    persisted.at("vertex_id") = vertex_id;
                    persisted.at("role") = target->start_vertex_id == vertex_id ? "start" : "end";
                }
                (void)decode_constraint_entity(entity);
            }
            if (entity.type != "dimension") continue;
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
            auto dimension = *decoded.dimension;
            if (dimension.boundary_id != edit.boundary_id) continue;
            if (dimension.kind == BoundaryDimensionKind::area) continue;
            if (topology_changed) {
                if (dimension.kind == BoundaryDimensionKind::segment_length &&
                    dimension.placement == BoundaryDimensionPlacement::automatic) {
                    if (!reviewed_room_owners.empty() && automatic_template &&
                        automatic_dimension_template_metadata(*automatic_template).dump()!=
                            automatic_dimension_template_metadata(source.at(id)).dump())
                        throw std::invalid_argument("Kept automatic dimensions have different styles or metadata. Remove the differing dimensions or review their targets individually.");
                    if (!automatic_template) automatic_template = &source.at(id);
                    retired_dimensions.push_back(id);
                } else if (removed_references.contains(id)) {
                    if (dimension.placement != BoundaryDimensionPlacement::manual) {
                        if (!edit.allow_automatic_angle_removal || dimension.kind != BoundaryDimensionKind::angle)
                            throw std::invalid_argument("Automatic angle removal requires explicit version-five redraw policy");
                        removed_automatic_angle = true;
                    }
                    accepted_removals.insert(id);
                } else {
                    // Replace only analytical targets. Re-encoding would
                    // normalize presentation numbers and unrelated metadata.
                    auto& target = entity.properties.at("target");
                    if (dimension.segment_chain_ids.empty()) {
                        target.at("segment_id") = mapped_child(dimension.segment_id, "segments");
                    } else {
                        auto& ids = target.at("segment_ids");
                        for (std::size_t i = 0; i < dimension.segment_chain_ids.size(); ++i)
                            ids.at(i) = mapped_child(dimension.segment_chain_ids[i], "segments");
                    }
                    if (dimension.kind == BoundaryDimensionKind::angle) {
                        target.at("second_segment_id") = mapped_child(dimension.secondary_segment_id, "segments");
                        target.at("vertex_id") = mapped_child(dimension.vertex_id, "vertices");
                    }
                    const auto remapped = decode_boundary_dimension_entity(entity);
                    (void)remapped.dimension->resolve(result);
                }
            } else {
                (void)dimension.resolve(result);
                if (dimension.kind == BoundaryDimensionKind::segment_length &&
                    dimension.placement == BoundaryDimensionPlacement::automatic) {
                    const auto side = dimension.automatic_placement_version.value_or(1) == 1 ? 1.0 :
                        (signed_area(boundary_geometry(edited)) > 0 ? -1.0 : 1.0);
                    dimension.text_position = split_dimension_position(dimension.resolve(result).segment, side);
                    entity = encode_boundary_dimension_entity(dimension, &entity);
                }
            }
        }
        const auto expected_count = automatic_template ? edited.segments.size() : 0;
        if (edit.replacement_dimension_ids.size() != expected_count)
            throw std::invalid_argument("Redefinition dimension IDs do not match its automatic placement policy");
        if (accepted_removals != removed_references)
            throw std::invalid_argument("Removed redraw references must be supported affected dimensions or endpoint constraints");
        if (edit.allow_automatic_angle_removal && !removed_automatic_angle)
            throw std::invalid_argument("Automatic angle removal policy must remove an affected automatic angle");
        const auto mapping_count = edit.replacement_child_mapping.empty() ? std::size_t{0} :
            edit.replacement_child_mapping.at("segments").size() + edit.replacement_child_mapping.at("vertices").size();
        if (used_mapping.size() != mapping_count)
            throw std::invalid_argument("Redraw child mapping contains unused reference decisions");
        for (const auto& id : accepted_removals) result.erase(id);
        for (const auto& id : retired_dimensions) result.erase(id);
        for (std::size_t i = 0; i < expected_count; ++i) {
            auto dimension = *decode_boundary_dimension_entity(*automatic_template).dimension;
            dimension.id = edit.replacement_dimension_ids[i];
            if (source.contains(dimension.id)) throw std::invalid_argument("Redefinition dimension ID is not fresh");
            dimension.segment_id = edited.segments[i].segment_id;
            dimension.segment_chain_ids.clear();
            const auto side = dimension.automatic_placement_version.value_or(1) == 1 ? 1.0 :
                (signed_area(boundary_geometry(edited)) > 0 ? -1.0 : 1.0);
            dimension.text_position = split_dimension_position(edited.segments[i].segment, side);
            auto entity = [&] {
                if (!reviewed_room_owners.empty()) {
                    auto template_entity = *automatic_template;
                    template_entity.id = dimension.id;
                    return encode_boundary_dimension_entity(dimension, &template_entity);
                }
                // Preserve the existing ordinary-command replay dialect.
                auto legacy=encode_boundary_dimension_entity(dimension);
                for (const auto* key:{"property_id","building_id","floor_id","layer_id"})
                    if (automatic_template->properties.contains(key)) legacy.properties[key]=automatic_template->properties.at(key);
                return legacy;
            }();
            result.emplace(entity.id, std::move(entity));
        }
    }
    if (!batch && edit.kind == BoundaryGeometryEditKind::reconstruct_arc) {
        // Curvature changes the analytical length and exterior midpoint even
        // though both endpoints remain fixed. Retain manual placements and
        // refresh automatic dimensions using their persisted winding policy.
        for (auto& [id, entity] : result) {
            (void)id;
            if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
            auto dimension = *decoded.dimension;
            if (dimension.boundary_id != edit.boundary_id) continue;
            (void)dimension.resolve(result);
            if (dimension.kind != BoundaryDimensionKind::segment_length ||
                dimension.placement != BoundaryDimensionPlacement::automatic) continue;
            const auto side = dimension.automatic_placement_version.value_or(1) == 1 ? 1.0 :
                (signed_area(boundary_geometry(edited)) > 0 ? -1.0 : 1.0);
            dimension.text_position = split_dimension_position(dimension.resolve(result).segment, side);
            entity = encode_boundary_dimension_entity(dimension, &entity);
        }
    }
    if (!batch && edit.kind == BoundaryGeometryEditKind::insert_vertex) {
        const auto old_boundary = decode_identified_boundary_entity(original);
        const auto old_edge = std::find_if(old_boundary.segments.begin(), old_boundary.segments.end(),
            [&](const auto& edge) { return edge.segment_id == edit.target_id; });
        const auto first_piece = std::find_if(edited.segments.begin(), edited.segments.end(),
            [&](const auto& edge) { return edge.segment_id == edit.target_id; });
        const auto second_piece = std::next(first_piece);
        const Entity* automatic_template = nullptr;
        for (auto& [id, entity] : result) {
            (void)id;
            if (entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (!decoded.supported())
                    throw std::invalid_argument(decoded.unsupported_reason);
                if (decoded.constraint->relation == ConstraintRelationKind::fixed_arc_length) {
                    auto bindings = nlohmann::json::array();
                    bool expanded = false;
                    const auto& old_bindings = decoded.constraint->bindings;
                    for (std::size_t i = 0; i < old_bindings.size(); i += 2) {
                        const auto& first = old_bindings[i];
                        const auto& second = old_bindings[i + 1];
                        const auto& first_json = entity.properties.at("bindings").at(i);
                        const auto& second_json = entity.properties.at("bindings").at(i + 1);
                        if (first.owner_id != edit.boundary_id || first.segment_id != edit.target_id) {
                            bindings.push_back(first_json);
                            bindings.push_back(second_json);
                            continue;
                        }
                        // Duplicate each original endpoint's opaque binding
                        // object before replacing only its canonical identity.
                        // The directed pair order is retained for reverse arcs.
                        const auto append_piece = [&](const IdentifiedSegment& piece) {
                            auto start = first_json;
                            auto end = second_json;
                            start["segment_id"] = piece.segment_id;
                            end["segment_id"] = piece.segment_id;
                            start["vertex_id"] = first.role == WallEndpointRole::start ?
                                piece.start_vertex_id : piece.end_vertex_id;
                            end["vertex_id"] = second.role == WallEndpointRole::start ?
                                piece.start_vertex_id : piece.end_vertex_id;
                            bindings.push_back(std::move(start));
                            bindings.push_back(std::move(end));
                        };
                        if (first.role == WallEndpointRole::start) {
                            append_piece(*first_piece);
                            append_piece(*second_piece);
                        } else {
                            append_piece(*second_piece);
                            append_piece(*first_piece);
                        }
                        expanded = true;
                    }
                    if (expanded) {
                        entity.properties.at("bindings") = std::move(bindings);
                        entity.properties.at("version") = 4;
                    }
                    const auto remapped = decode_constraint_entity(entity);
                    (void)resolve_constraint_arc_length(*remapped.constraint, result);
                    continue;
                }
                // Known constraints are point relations. Preserve their exact
                // endpoint points and opaque binding metadata, including spans
                // that now traverse both pieces rather than one shorter edge.
                for (std::size_t i = 0; i < decoded.constraint->bindings.size(); ++i) {
                    const auto& binding = decoded.constraint->bindings[i];
                    if (binding.owner_id == edit.boundary_id && binding.segment_id == edit.target_id &&
                        binding.role == WallEndpointRole::end && binding.vertex_id == old_edge->end_vertex_id)
                        entity.properties.at("bindings").at(i).at("segment_id") = edit.new_segment_id;
                }
                (void)decode_constraint_entity(entity);
            }
            if (entity.type != "dimension") continue;
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
            auto dimension = *decoded.dimension;
            if (dimension.boundary_id != edit.boundary_id) continue;
            if (dimension.kind == BoundaryDimensionKind::angle &&
                dimension.vertex_id == old_edge->end_vertex_id) {
                if (dimension.segment_id == edit.target_id) dimension.segment_id = edit.new_segment_id;
                if (dimension.secondary_segment_id == edit.target_id)
                    dimension.secondary_segment_id = edit.new_segment_id;
                entity = encode_boundary_dimension_entity(dimension, &entity);
            }
            if (dimension.kind == BoundaryDimensionKind::angle)
                (void)dimension.resolve(result);
            if (dimension.kind == BoundaryDimensionKind::segment_length &&
                (!dimension.segment_chain_ids.empty() || dimension.placement == BoundaryDimensionPlacement::manual)) {
                auto ids = dimension.segment_chain_ids;
                if (ids.empty()) ids.push_back(dimension.segment_id);
                const auto member = std::find(ids.begin(), ids.end(), edit.target_id);
                if (member != ids.end()) {
                    ids.insert(std::next(member), edit.new_segment_id);
                    dimension.segment_chain_ids = std::move(ids);
                    entity = encode_boundary_dimension_entity(dimension, &entity);
                }
                (void)dimension.resolve(result);
                continue;
            }
            if (dimension.kind == BoundaryDimensionKind::segment_length &&
                dimension.segment_id == edit.target_id &&
                dimension.placement == BoundaryDimensionPlacement::automatic) {
                if (!automatic_template) automatic_template = &source.at(id);
                const auto side = dimension.automatic_placement_version.value_or(1) == 1 ? 1.0 :
                    (signed_area(boundary_geometry(edited)) > 0.0 ? -1.0 : 1.0);
                dimension.text_position = split_dimension_position(first_piece->segment, side);
                entity = encode_boundary_dimension_entity(dimension, &entity);
            }
        }
        if (automatic_template) {
            if (edit.new_dimension_id.empty() || source.contains(edit.new_dimension_id))
                throw std::invalid_argument("Automatic split dimension requires a fresh explicit ID");
            auto dimension = *decode_boundary_dimension_entity(*automatic_template).dimension;
            dimension.id = edit.new_dimension_id;
            dimension.segment_id = edit.new_segment_id;
            const auto side = dimension.automatic_placement_version.value_or(1) == 1 ? 1.0 :
                (signed_area(boundary_geometry(edited)) > 0.0 ? -1.0 : 1.0);
            dimension.text_position = split_dimension_position(second_piece->segment, side);
            auto new_dimension = encode_boundary_dimension_entity(dimension);
            for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                if (automatic_template->properties.contains(key))
                    new_dimension.properties[key] = automatic_template->properties.at(key);
            result.emplace(new_dimension.id, std::move(new_dimension));
        } else if (!edit.new_dimension_id.empty()) {
            throw std::invalid_argument("Split dimension ID has no automatic source dimension");
        }
    }
    return result;
}

std::map<std::string, Entity, std::less<>> edited_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source, const BoundaryGeometryEdit& edit) {
    return edited_boundary_entities_impl(source, edit, nullptr);
}

std::map<std::string, Entity, std::less<>> edited_boundary_entities_for_room_review(
    const std::map<std::string, Entity, std::less<>>& source, const BoundaryGeometryEdit& edit,
    const std::set<std::string>& reviewed_owners) {
    if (!edit.physical_wall_room_repair || !reviewed_owners.contains(edit.boundary_id))
        throw std::invalid_argument("Complete room-review reconstruction requires its retained ownership scope");
    return edited_boundary_entities_impl(source,edit,nullptr,false,reviewed_owners);
}

std::map<std::string, Entity, std::less<>> replayed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source, const BoundaryGeometryEdit& edit) {
    return edited_boundary_entities_impl(source, edit, nullptr, true);
}

static std::map<std::string, Entity, std::less<>> edited_boundary_entities_batch_impl(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits, bool retained_replay) {
    // Preserve already persisted format-8 proofs exactly whenever sequential
    // replay is valid. An invalid intermediate state is never published.
    try {
        auto sequential = source;
        for (const auto& edit : edits)
            sequential = edited_boundary_entities_impl(sequential, edit, nullptr, retained_replay);
        return sequential;
    } catch (const std::invalid_argument&) {
        std::map<std::string, std::vector<BoundaryGeometryEdit>, std::less<>> groups;
        for (const auto& edit : edits) groups[edit.boundary_id].push_back(edit);
        auto result = source;
        for (const auto& [id, group] : groups) {
            const bool vertices_only = std::all_of(group.begin(), group.end(), [](const auto& edit) {
                return edit.kind == BoundaryGeometryEditKind::move_vertex;
            });
            if (vertices_only) {
                result = edited_boundary_entities_impl(result, group.front(), &group, retained_replay);
            } else {
                // A different owner may require simultaneous vertex movement.
                // Keep semantic resize/insertion/redraw proofs replayable in
                // their original order instead of treating them as vertex edits.
                for (const auto& edit : group)
                    result = edited_boundary_entities_impl(result, edit, nullptr, retained_replay);
            }
        }
        (void)validate_boundary_integrity(result);
        return result;
    }
}

std::map<std::string, Entity, std::less<>> edited_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits) {
    return edited_boundary_entities_batch_impl(source, edits, false);
}

std::map<std::string, Entity, std::less<>> replayed_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits) {
    return edited_boundary_entities_batch_impl(source, edits, true);
}

std::optional<std::string> validate_boundary_integrity(
    const std::map<std::string, Entity, std::less<>>& entities) {
    std::optional<std::string> unsupported;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> edges;
    std::set<std::string, std::less<>> future_boundaries;
    for (const auto& [id, entity] : entities) {
        if (entity.type == "measurement_linework") {
            if (!entity.properties.contains("model"))
                throw std::invalid_argument("Measured stroke " + id + ": missing model");
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            if (!decoded.supported()) {
                future_boundaries.insert(id);
                if (!unsupported) unsupported = "Measured stroke " + id + ": " + decoded.diagnostic;
            } else if (decoded.model->stroke_id != entity.id)
                throw std::invalid_argument("Measured stroke " + id + ": model owner identity differs");
            continue;
        }
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
            if (entity.properties.contains("wall_measurement_source") &&
                entity.properties.at("wall_measurement_source").is_object() &&
                entity.properties.at("wall_measurement_source").contains("version") &&
                entity.properties.at("wall_measurement_source").at("version")==2) {
                const auto physical=materialize_exterior_wall_measurement(entity);
                const auto actual=boundary_geometry(boundary);
                if (actual.size()!=physical.boundary.size()) throw std::invalid_argument("Physical translation lineage changed topology");
                for (std::size_t i=0;i<actual.size();++i) {
                    const auto& a=actual[i]; const auto& b=physical.boundary[i];
                    if (a.start.x!=b.start.x || a.start.y!=b.start.y || a.end.x!=b.end.x || a.end.y!=b.end.y ||
                        a.sweep_radians!=b.sweep_radians)
                        throw std::invalid_argument("Physical translation lineage differs from the retained outline");
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
                // Reconcile the retained proof with the final source envelope,
                // without requiring historical walls to remain in today's map.
                std::vector<std::string> reviewed_sources;
                std::optional<nlohmann::json> reviewed_linework;
                std::optional<PhysicalWallRoomRepairIntent> reviewed_room;
                std::optional<nlohmann::json> reviewed_room_descriptor;
                for (const auto& operation : entity.extensions.at("boundary_geometry_derivation").at("operations")) {
                    if (operation.at("kind") == "physical_room_wall_merge") {
                        const auto& value = operation.at("value");
                        const auto& captured = value.at("source_descriptor");
                        if (reviewed_room_descriptor && *reviewed_room_descriptor != captured)
                            throw std::invalid_argument("Boundary " + id + ": physical room merge source breaks its retained descriptor chain");
                        if (reviewed_room && (captured.at("selected_wall_id") != reviewed_room->selected_wall_id ||
                            captured.at("source_lineage") != reviewed_room->reviewed_source_lineage))
                            throw std::invalid_argument("Boundary " + id + ": physical room merge source differs from its preceding repair");
                        reviewed_room_descriptor = value.at("descriptor");
                        reviewed_room.reset();
                        continue;
                    }
                    if(operation.at("kind")=="wall_merge") {
                        auto merged_sources=operation.at("value").at("wall_source_ids").get<std::vector<std::string>>();
                        if(!reviewed_sources.empty()) {
                            const auto retired=operation.at("value").at("removed_wall_id").get<std::string>();
                            const auto member=std::find(reviewed_sources.begin(),reviewed_sources.end(),retired);
                            if(member==reviewed_sources.end())throw std::invalid_argument("Wall merge boundary proof did not retire a recorded source");
                            reviewed_sources.erase(member);
                            std::sort(reviewed_sources.begin(),reviewed_sources.end());std::sort(merged_sources.begin(),merged_sources.end());
                            if(reviewed_sources!=merged_sources)throw std::invalid_argument("Wall merge boundary proof changed unrelated physical sources");
                        }
                        reviewed_sources=std::move(merged_sources);
                        continue;
                    }
                    if (operation.at("kind") != "geometry_edit") continue;
                    const auto edit = decode_boundary_geometry_edit(operation.at("value"));
                    if (!edit.replacement_wall_source_ids.empty()) reviewed_sources = edit.replacement_wall_source_ids;
                    if (edit.replacement_linework_sources) reviewed_linework = edit.replacement_linework_sources;
                    if (edit.physical_wall_room_repair) {
                        if (reviewed_room_descriptor) {
                            auto captured = entity;
                            captured.extensions["physical_wall_room"] = *reviewed_room_descriptor;
                            if (physical_wall_room_descriptor_digest(captured) != edit.physical_wall_room_repair->expected_descriptor_digest)
                                throw std::invalid_argument("Boundary " + id + ": physical room repair source differs from its preceding merge");
                        }
                        reviewed_room = edit.physical_wall_room_repair;
                        reviewed_room_descriptor.reset();
                    }
                }
                if (reviewed_room_descriptor && (!is_physical_wall_room(entity) ||
                    entity.extensions.at("physical_wall_room") != *reviewed_room_descriptor))
                    throw std::invalid_argument("Boundary " + id + ": physical room source differs from its retained merge descriptor");
                if (reviewed_room) {
                    const auto descriptor=decode_physical_wall_room_descriptor(entity);
                    if (descriptor.selected_wall_id!=reviewed_room->selected_wall_id ||
                        descriptor.source_lineage!=reviewed_room->reviewed_source_lineage)
                        throw std::invalid_argument("Boundary " + id + ": physical room source differs from its retained reviewed repair proof");
                }
                if (!reviewed_sources.empty()) {
                    auto actual_sources = exterior_wall_measurement_source_ids(entity);
                    std::sort(reviewed_sources.begin(), reviewed_sources.end());
                    std::sort(actual_sources.begin(), actual_sources.end());
                    if (reviewed_sources != actual_sources)
                        throw std::invalid_argument("Boundary " + id + ": exterior source differs from its reviewed replacement proof");
                }
                if (reviewed_linework && (entity.type != "measurement_boundary" ||
                    entity.properties.contains("wall_measurement_source") ||
                    !entity.extensions.contains("measurement_linework_sources") ||
                    entity.extensions.at("measurement_linework_sources").dump() != reviewed_linework->dump()))
                    throw std::invalid_argument("Boundary " + id + ": measured-line sources differ from their reviewed replacement proof");
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
        if (dimension.kind == BoundaryDimensionKind::wall_axis_length) {
            if (owner == entities.end() || owner->second.type != "wall")
                throw std::invalid_argument("Dimension " + id + ": missing or invalid physical wall owner");
            validate_boundary_dimension_target(dimension, owner->second);
            continue;
        }
        if (owner == entities.end() || (!can_recognize_boundary_entity_type(owner->second.type) &&
                                      owner->second.type != "measurement_linework"))
            throw std::invalid_argument("Dimension " + id + ": missing or invalid boundary owner");
        if (owner->second.type == "measurement_linework" && dimension.kind == BoundaryDimensionKind::area)
            throw std::invalid_argument("Dimension " + id + ": area dimensions cannot target measured strokes");
        // The future geometry is opaque. Preserve a well-formed reference in
        // the read-only document without pretending to resolve its child IDs.
        if (future_boundaries.contains(owner->first)) continue;
        if (owner->second.type == "measurement_linework") {
            // Full analytical resolution also checks shared vertex identity and
            // nondegenerate tangents; edge membership alone is insufficient.
            validate_boundary_dimension_target(dimension,owner->second);
            continue;
        }
        const auto boundary_edges = edges.find(owner->first);
        if (boundary_edges == edges.end())
            throw std::invalid_argument("Dimension " + id + ": missing identified source boundary");
        if (dimension.kind == BoundaryDimensionKind::segment_length) {
            if (!boundary_edges->second.contains(dimension.segment_id))
                throw std::invalid_argument("Dimension " + id + ": missing identified source segment");
            if (!dimension.segment_chain_ids.empty())
                validate_boundary_dimension_target(dimension,owner->second);
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
        if (identified_v1(entity) && entity.type == "measurement_boundary" &&
            (entity.properties.contains("wall_measurement_source") != found->second.properties.contains("wall_measurement_source") ||
             (entity.properties.contains("wall_measurement_source") &&
              entity.properties.at("wall_measurement_source") != found->second.properties.at("wall_measurement_source"))))
            throw std::invalid_argument("Exterior measured-area sources require a reviewed typed source replacement");
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
