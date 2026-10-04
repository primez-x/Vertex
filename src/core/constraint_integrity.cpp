#include "sketch/constraint_integrity.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <limits>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr double linear_tolerance = constraint_linear_tolerance_metres;
constexpr double angular_tolerance = constraint_angular_tolerance_radians;

struct IndexedOpening {
    std::string id;
    const Entity* entity = nullptr;
};

using OpeningIndex = std::map<std::string, std::vector<IndexedOpening>, std::less<>>;

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument(message);
}

double number(const json& value, const char* description) {
    if (!value.is_number()) invalid(std::string(description) + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string(description) + " must be finite");
    return result;
}

Vec2 point(const json& value, const char* description) {
    if (!value.is_array() || value.size() != 2)
        invalid(std::string(description) + " must have two coordinates");
    return {number(value[0], description), number(value[1], description)};
}

OpeningIndex index_openings(const Entities& entities) {
    OpeningIndex result;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "opening" || !entity.properties.is_object()) continue;
        const auto host = entity.properties.find("wall_id");
        if (host == entity.properties.end() || !host->is_string()) continue;
        result[host->get_ref<const std::string&>()].push_back({id, &entity});
    }
    return result;
}

Wall read_wall(const std::string& owner, const Entities& entities,
               const OpeningIndex& openings_by_wall) {
    const auto found = entities.find(owner);
    if (found == entities.end() || found->second.type != "wall")
        invalid("Constraint owner is not an existing wall: " + owner);
    const auto& properties = found->second.properties;
    const auto& baseline = properties.at("baseline");
    Wall wall{owner,
        {point(baseline.at("start"), "Wall start"), point(baseline.at("end"), "Wall end"),
         number(baseline.at("sweep_radians"), "Wall sweep")},
        number(properties.at("thickness_m"), "Wall thickness"),
        number(properties.at("height_m"), "Wall height"),
        number(properties.at("elevation_m"), "Wall elevation"), {}};
    if (const auto layers = properties.find("layers"); layers != properties.end()) {
        wall.layers = parse_wall_layers(layers.value(), wall.thickness);
    }
    if (const auto slope = properties.find("slope_rise_m"); slope != properties.end()) {
        if (!slope->is_number()) invalid("Wall slope_rise_m must be a finite number");
        wall.slope_rise = slope->get<double>();
    }
    const auto hosted = openings_by_wall.find(owner);
    if (hosted != openings_by_wall.end()) {
        wall.openings.reserve(hosted->second.size());
        for (const auto& indexed : hosted->second) {
            const auto& p = indexed.entity->properties;
            wall.openings.push_back({indexed.id, number(p.at("offset_m"), "Opening offset"),
                number(p.at("width_m"), "Opening width"),
                number(p.at("sill_m"), "Opening sill"),
                number(p.at("height_m"), "Opening height")});
        }
    }
    validate_wall_semantics(wall);
    validate_wall_curve_input(found->second);
    return wall;
}

std::vector<std::string> typed_wall_ids(const Entity& entity) {
    const auto& value = entity.properties.at("wall_ids");
    std::vector<std::string> result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(item.get_ref<const std::string&>());
    return result;
}

Vec2 difference(Vec2 first, Vec2 second) {
    const Vec2 result{first.x - second.x, first.y - second.y};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        invalid("Constraint displacement exceeds the supported numeric range");
    return result;
}

double length(Vec2 value) {
    const auto result = std::hypot(value.x, value.y);
    if (!std::isfinite(result)) invalid("Constraint length exceeds the supported numeric range");
    return result;
}

Vec2 direction(Vec2 start, Vec2 end) {
    const auto delta = difference(end, start);
    const auto magnitude = length(delta);
    if (magnitude <= default_geometry_tolerance_metres)
        invalid("Constraint direction endpoints must be distinct");
    return {delta.x / magnitude, delta.y / magnitude};
}

bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

WallEndpointRole reverse_role(WallEndpointRole role) {
    return role == WallEndpointRole::start ? WallEndpointRole::end : WallEndpointRole::start;
}
} // namespace

std::optional<std::string> validate_constraint_integrity(const Entities& entities) {
    std::optional<std::string> unsupported;
    std::map<std::string, Wall, std::less<>> owners;
    std::set<std::string, std::less<>> opaque_strokes;
    for (const auto& [id, owner] : entities) {
        if (owner.type != "measurement_linework") continue;
        const auto decoded = decode_measurement_linework_model(owner.properties.at("model"));
        if (!decoded.supported()) {
            opaque_strokes.insert(id);
            if (!unsupported) unsupported = "Measured constraint owner " + id + ": " + decoded.diagnostic;
        }
    }
    const auto openings_by_wall = index_openings(entities);
    for (const auto& [id, entity] : entities) {
        if (entity.type != "constraint") continue;
        try {
            const auto decoded = decode_constraint_entity(entity);
            for (const auto& owner_id : entity.properties.contains("wall_ids")
                     ? typed_wall_ids(entity) : std::vector<std::string>{}) {
                auto found = owners.find(owner_id);
                if (found == owners.end()) {
                    found = owners.emplace(owner_id,
                        read_wall(owner_id, entities, openings_by_wall)).first;
                }
            }
            if (!decoded.constraint) {
                if (entity.properties.contains("entity_ids")) {
                    for (const auto& value : entity.properties.at("entity_ids")) {
                        const auto owner = entities.find(value.get<std::string>());
                        if (owner == entities.end()) invalid("Constraint owner does not exist");
                        if (owner->second.type == "wall")
                            (void)read_wall(owner->first, entities, openings_by_wall);
                        else if (!opaque_strokes.contains(owner->first))
                            (void)resolve_constraint_segment_owner(owner->second);
                    }
                }
                if (!unsupported) unsupported = "Constraint " + id + ": " + decoded.unsupported_reason;
                continue;
            }
            const auto& constraint = *decoded.constraint;
            std::vector<Vec2> points;
            bool opaque_owner = false;
            for (const auto& binding : constraint.bindings) {
                if (!binding.segment_id.empty()) {
                    const auto owner = entities.find(binding.owner_id);
                    if (owner == entities.end()) invalid("Boundary constraint owner is missing");
                    if (opaque_strokes.contains(binding.owner_id)) {
                        opaque_owner = true;
                        continue;
                    }
                    const auto boundary = resolve_constraint_segment_owner(owner->second);
                    const auto edge = std::find_if(boundary.segments.begin(), boundary.segments.end(),
                        [&](const auto& value) { return value.segment_id == binding.segment_id; });
                    if (edge == boundary.segments.end()) invalid("Boundary constraint segment is missing");
                    const bool start = binding.role == WallEndpointRole::start;
                    if ((start ? edge->start_vertex_id : edge->end_vertex_id) != binding.vertex_id)
                        invalid("Boundary constraint endpoint identity does not match its segment");
                    points.push_back(start ? edge->segment.start : edge->segment.end);
                    continue;
                }
                auto found = owners.find(binding.owner_id);
                if (found == owners.end())
                    found = owners.emplace(binding.owner_id,
                        read_wall(binding.owner_id, entities, openings_by_wall)).first;
                points.push_back(binding.role == WallEndpointRole::start
                    ? found->second.baseline.start : found->second.baseline.end);
            }
            // Keep an unknown measured model opaque while validating every
            // other known owner and every unrelated known relation.
            if (opaque_owner) continue;
            bool satisfied = false;
            switch (constraint.relation) {
            case ConstraintRelationKind::horizontal:
                satisfied = std::abs(difference(points.at(1), points.at(0)).y) <= linear_tolerance;
                break;
            case ConstraintRelationKind::vertical:
                satisfied = std::abs(difference(points.at(1), points.at(0)).x) <= linear_tolerance;
                break;
            case ConstraintRelationKind::coincident:
                satisfied = length(difference(points.at(1), points.at(0))) <= linear_tolerance;
                break;
            case ConstraintRelationKind::fixed_length:
                satisfied = constraint.length.has_value() &&
                    std::abs(length(difference(points.at(1), points.at(0))) -
                             constraint.length->metres) <= linear_tolerance;
                break;
            case ConstraintRelationKind::fixed_arc_length:
                (void)constraint_arc_chord_target(constraint,entities);
                satisfied = constraint.length.has_value() &&
                    std::abs(segment_length(resolve_constraint_arc_segment(constraint,entities))-
                        constraint.length->metres)<=linear_tolerance;
                break;
            case ConstraintRelationKind::fixed_anchor:
                satisfied = constraint.anchor.has_value() &&
                    length(difference(points.at(0), *constraint.anchor)) <= linear_tolerance;
                break;
            case ConstraintRelationKind::parallel:
            case ConstraintRelationKind::perpendicular: {
                const auto first = direction(points.at(0), points.at(1));
                const auto second = direction(points.at(2), points.at(3));
                const auto cross = std::abs(first.x * second.y - first.y * second.x);
                const auto dot = std::abs(first.x * second.x + first.y * second.y);
                const auto residual = constraint.relation == ConstraintRelationKind::parallel
                    ? std::atan2(cross, dot) : std::atan2(dot, cross);
                satisfied = std::isfinite(residual) && residual <= angular_tolerance;
                break;
            }
            }
            if (!satisfied) invalid("Persisted hard relation is not satisfied");
        } catch (const std::exception& error) {
            invalid("Constraint " + id + ": " + error.what());
        }
    }
    return unsupported;
}

void validate_constraint_transition(const Entities& before, const Entities& after,
                                   bool qualified_rigid_endpoint_transform,
                                   const std::set<std::string,std::less<>>& verified_rigid_wall_ids) {
    // A verified rigid transform moves named endpoints without reversing
    // their identities, even when its coordinates exchange start and end.
    // Exact history navigation likewise restores those retained identities.
    if (qualified_rigid_endpoint_transform)
        return;
    std::set<std::string, std::less<>> reversed;
    for (const auto& [id, entity] : before) {
        if (entity.type != "constraint" || !after.contains(id) ||
            after.at(id).type != "constraint") continue;
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.constraint) continue; // Unsupported documents are read-only.
        for (const auto& binding : decoded.constraint->bindings) {
            if (!binding.segment_id.empty()) continue;
            if (verified_rigid_wall_ids.contains(binding.owner_id) || reversed.contains(binding.owner_id) || !after.contains(binding.owner_id)) continue;
            const auto& old_baseline = before.at(binding.owner_id).properties.at("baseline");
            const auto& new_baseline = after.at(binding.owner_id).properties.at("baseline");
            if (same_point(point(old_baseline.at("start"), "Wall start"),
                           point(new_baseline.at("end"), "Wall end")) &&
                same_point(point(old_baseline.at("end"), "Wall end"),
                           point(new_baseline.at("start"), "Wall start")))
                reversed.insert(binding.owner_id);
        }
    }
    if (reversed.empty()) return;
    for (const auto& [id, entity] : before) {
        if (entity.type != "constraint" || !after.contains(id) ||
            after.at(id).type != "constraint") continue;
        const auto old_value = decode_constraint_entity(entity);
        if (!old_value.constraint) continue;
        const auto new_value = decode_constraint_entity(after.at(id));
        if (!new_value.constraint) invalid("Wall reversal cannot introduce unknown lock semantics");
        const auto& old_bindings = old_value.constraint->bindings;
        const auto& new_bindings = new_value.constraint->bindings;
        for (std::size_t index = 0; index < old_bindings.size(); ++index) {
            const auto& binding = old_bindings[index];
            if (!reversed.contains(binding.owner_id)) continue;
            if (index >= new_bindings.size() || new_bindings[index].owner_id != binding.owner_id ||
                new_bindings[index].role != reverse_role(binding.role))
                invalid("Wall reversal requires atomic remapping of constraint " + id);
        }
    }
}

namespace {
bool topology_physical_wall(const Entity& owner) {
    return owner.type=="wall" && owner.properties.is_object() && owner.properties.contains("baseline") &&
        owner.properties.contains("thickness_m") && owner.properties.contains("height_m") &&
        owner.properties.contains("elevation_m");
}
Segment topology_baseline(const Entity& owner) {
    const auto& value=owner.properties.at("baseline");
    return {point(value.at("start"),"Wall start"),point(value.at("end"),"Wall end"),
        number(value.at("sweep_radians"),"Wall sweep")};
}

bool topology_near(Vec2 a,Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<=linear_tolerance; }

std::string topology_point(const WallEndpointBinding& binding) {
    const auto owner=std::to_string(binding.owner_id.size())+":"+binding.owner_id;
    return binding.vertex_id.empty() ? owner+(binding.role==WallEndpointRole::start ? ":start" : ":end")
        : owner+":vertex:"+binding.vertex_id;
}

struct TopologyPoints {
    std::map<std::string,std::string,std::less<>> parent;
    std::string root(const std::string& id) {
        const auto [where,inserted]=parent.try_emplace(id,id);
        if (!inserted && where->second!=id) where->second=root(where->second);
        return where->second;
    }
    void join(const std::string& first,const std::string& second) {
        auto a=root(first); auto b=root(second);
        if (a!=b) { if (b<a) std::swap(a,b); parent[b]=a; }
    }
};

int contact_role(const Segment& segment,Vec2 value) {
    if (topology_near(segment.start,value)) return 0;
    if (topology_near(segment.end,value)) return 1;
    return 2;
}

bool topology_same_plane(const Entity& first,const Entity& second,const ProjectOrganization& organization) {
    const auto context=[&](const Entity& entity) {
        const auto resolved=organization.drawing_context(entity.id);
        if ((entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
             entity.properties.contains("building_id") || entity.properties.contains("property_id")) && !resolved)
            invalid("Wall has an unresolved explicit drawing context: "+entity.id);
        return resolved;
    };
    const auto a=context(first); const auto b=context(second);
    if (a && b && a->floor_id!=b->floor_id) return false;
    const long double low_a=number(first.properties.at("elevation_m"),"Wall elevation");
    const long double low_b=number(second.properties.at("elevation_m"),"Wall elevation");
    const auto high_a=low_a+number(first.properties.at("height_m"),"Wall height");
    const auto high_b=low_b+number(second.properties.at("height_m"),"Wall height");
    if (!std::isfinite(high_a) || !std::isfinite(high_b)) invalid("Wall vertical extent exceeds supported range");
    return std::max(low_a,low_b)<std::min(high_a,high_b)+linear_tolerance;
}

void validate_topology_cycle(const std::vector<std::pair<std::string,bool>>& edges,
    const Entities& before,const Entities& after) {
    const auto loop=[&](const Entities& entities) {
        Boundary result;
        for (const auto& [id,forward] : edges) {
            auto segment=topology_baseline(entities.at(id));
            if (!forward) { std::swap(segment.start,segment.end); segment.sweep_radians=-segment.sweep_radians; }
            result.push_back(segment);
        }
        return result;
    };
    const auto old_loop=loop(before); const auto new_loop=loop(after);
    // A newly declared coincidence cycle need not have been closed before
    // solving. Protect existing valid cycles without inventing old closure.
    if (!validate_boundary(old_loop,linear_tolerance).empty()) return;
    const auto old_area=signed_area(old_loop); const auto area=signed_area(new_loop);
    if (!validate_boundary(new_loop,linear_tolerance).empty() || !std::isfinite(area) ||
        std::abs(area)<=linear_tolerance*linear_tolerance || (old_area>0)!=(area>0))
        invalid("Constraint edit would change analytical wall-cycle winding or topology");
}
}

void validate_constraint_edit_topology(const Entities& before,const Entities& after,
    const std::set<std::string,std::less<>>& verified_rigid_wall_ids) {
    std::set<std::string,std::less<>> changed_walls;
    for (const auto& [id,owner] : after) {
        const auto previous=before.find(id);
        if (previous==before.end() || owner==previous->second) continue;
        if (topology_physical_wall(owner)) {
            if (previous->second.type!="wall") invalid("Constraint edit changed wall owner type");
            if (owner.properties.at("baseline")!=previous->second.properties.at("baseline")) {
                const auto old=topology_baseline(previous->second); const auto current=topology_baseline(owner);
                if (!verified_rigid_wall_ids.contains(id) && topology_near(old.start,current.end) && topology_near(old.end,current.start))
                    invalid("Constraint edit would reverse wall endpoint identity: "+id);
                changed_walls.insert(id);
            }
        } else if (can_recognize_boundary_entity_type(owner.type)) {
            const auto old=decode_identified_boundary_entity(previous->second);
            const auto current=decode_identified_boundary_entity(owner);
            const auto area=signed_area(boundary_geometry(current));
            if (!std::isfinite(area) || std::abs(area)<=linear_tolerance*linear_tolerance ||
                (signed_area(boundary_geometry(old))>0)!=(area>0))
                invalid("Constraint edit would change analytical boundary winding");
        }
    }
    if (changed_walls.empty()) return;
    const auto organization=organize_project(after);
    TopologyPoints points;
    for (const auto& [id,owner] : after) {
        (void)id;
        if (owner.type!="constraint") continue;
        const auto decoded=decode_constraint_entity(owner);
        if (decoded.constraint && decoded.constraint->relation==ConstraintRelationKind::coincident)
            points.join(topology_point(decoded.constraint->bindings.at(0)),topology_point(decoded.constraint->bindings.at(1)));
    }
    std::set<std::pair<std::string,std::string>> checked;
    for (const auto& id : changed_walls) {
        for (const auto& [other,owner] : after) {
            if (id==other || !topology_physical_wall(owner)) continue;
            const auto pair=std::minmax(id,other);
            if (!checked.emplace(pair.first,pair.second).second) continue;
            if (!before.contains(other) || before.at(other).type!="wall")
                invalid("Constraint edit cannot change topology across unknown wall owners");
            if (!topology_same_plane(after.at(id),owner,organization)) continue;
            const auto old_a=topology_baseline(before.at(id)); const auto old_b=topology_baseline(before.at(other));
            const auto a=topology_baseline(after.at(id)); const auto b=topology_baseline(owner);
            const auto old_hit=segment_intersection(old_a,old_b,linear_tolerance);
            const auto hit=segment_intersection(a,b,linear_tolerance);
            using Kind=SegmentIntersectionKind;
            if (old_hit.kind==Kind::indeterminate || hit.kind==Kind::indeterminate)
                invalid("Constraint wall topology has indeterminate analytical intersections");
            if ((old_hit.kind==Kind::proper)!=(hit.kind==Kind::proper) ||
                (old_hit.kind==Kind::overlap)!=(hit.kind==Kind::overlap) ||
                (old_hit.kind==Kind::proper && old_hit.points.size()!=hit.points.size()))
                invalid("Constraint edit would change wall crossing or overlap topology");
            // Overlap endpoints bound the common interval; they are not
            // isolated contact witnesses or new endpoint connections.
            if (hit.kind==Kind::overlap) continue;
            const auto proper_count=[](const SegmentIntersection& hits,const Segment& first,const Segment& second) {
                return std::count_if(hits.points.begin(),hits.points.end(),[&](const auto& value) {
                    return contact_role(first,value)==2 && contact_role(second,value)==2;
                });
            };
            if (proper_count(old_hit,old_a,old_b)!=proper_count(hit,a,b))
                invalid("Constraint edit would change the number of analytical wall crossings");
            std::set<std::pair<int,int>> old_contacts;
            for (const auto& value : old_hit.points) {
                const auto first=contact_role(old_a,value); const auto second=contact_role(old_b,value);
                if (first!=2 || second!=2) old_contacts.emplace(first,second);
            }
            for (const auto& value : hit.points) {
                const auto first=contact_role(a,value); const auto second=contact_role(b,value);
                if (first==2 && second==2) continue;
                if (old_contacts.contains({first,second})) continue;
                if (first==2 || second==2 ||
                    points.root(topology_point({id,first==0 ? WallEndpointRole::start : WallEndpointRole::end}))!=
                    points.root(topology_point({other,second==0 ? WallEndpointRole::start : WallEndpointRole::end})))
                    invalid("Constraint edit would create an implicit coordinate-only wall connection");
            }
        }
    }
    // Follow source and candidate cycles in actual shared drawing planes.
    // Each active interval set samples a lower Z endpoint, so every group
    // having a common vertical overlap is represented without merging floors
    // or relying on non-transitive pairwise height overlap.
    std::set<std::string> floors;
    std::map<std::string,std::optional<std::string>,std::less<>> wall_floors;
    std::map<std::string,std::pair<long double,long double>,std::less<>> spans;
    std::set<long double> levels;
    for (const auto& [id,owner] : after) {
        if (!topology_physical_wall(owner) || !before.contains(id) || !topology_physical_wall(before.at(id))) continue;
        const auto context=organization.drawing_context(id);
        if ((owner.properties.contains("floor_id") || owner.properties.contains("layer_id") ||
            owner.properties.contains("building_id") || owner.properties.contains("property_id")) && !context)
            invalid("Wall has an unresolved explicit drawing context: "+id);
        wall_floors[id]=context ? std::optional<std::string>{context->floor_id} : std::nullopt;
        if (context) floors.insert(context->floor_id);
        const long double low=number(owner.properties.at("elevation_m"),"Wall elevation");
        const auto high=low+number(owner.properties.at("height_m"),"Wall height")+linear_tolerance;
        if (!std::isfinite(high)) invalid("Wall vertical extent exceeds supported range");
        spans[id]={low,high}; levels.insert(low);
    }
    floors.insert(""); // Legacy unbound walls form their own applicable group.
    std::erase_if(levels,[&](const auto level) {
        return std::none_of(changed_walls.begin(),changed_walls.end(),[&](const auto& id) {
            const auto& span=spans.at(id); return span.first<=level && level<span.second;
        });
    });
    std::erase_if(floors,[&](const auto& floor) {
        return std::none_of(changed_walls.begin(),changed_walls.end(),[&](const auto& id) {
            const auto& selected_floor=wall_floors.at(id);
            if (selected_floor) return *selected_floor==floor;
            if (floor.empty()) return true;
            const auto& selected_span=spans.at(id);
            return std::any_of(spans.begin(),spans.end(),[&](const auto& other) {
                const auto& other_floor=wall_floors.at(other.first);
                return other_floor && *other_floor==floor &&
                    std::max(selected_span.first,other.second.first)<std::min(selected_span.second,other.second.second);
            });
        });
    });
    std::set<std::vector<std::string>> groups;
    for (const auto& floor : floors) for (const auto level : levels) {
        std::vector<std::string> group;
        for (const auto& [id,span] : spans) {
            const auto& owner_floor=wall_floors.at(id);
            if ((!owner_floor || (!floor.empty() && *owner_floor==floor)) && span.first<=level && level<span.second)
                group.push_back(id);
        }
        if (std::any_of(group.begin(),group.end(),[&](const auto& id) { return changed_walls.contains(id); }))
            groups.insert(std::move(group));
    }
    const auto validate_cycles=[&](const Entities& relations,const std::vector<std::string>& group) {
    TopologyPoints cycle_points;
    for (const auto& [id,owner] : relations) {
        (void)id;
        if (owner.type!="constraint") continue;
        const auto decoded=decode_constraint_entity(owner);
        if (decoded.constraint && decoded.constraint->relation==ConstraintRelationKind::coincident)
            cycle_points.join(topology_point(decoded.constraint->bindings.at(0)),topology_point(decoded.constraint->bindings.at(1)));
    }
    // Boundary vertex identities may provide transitive coincidence closure.
    struct Edge { std::string id,first,second; };
    std::vector<Edge> edges;
    for (const auto& id : group) {
        const auto first=cycle_points.root(topology_point({id,WallEndpointRole::start}));
        const auto second=cycle_points.root(topology_point({id,WallEndpointRole::end}));
        if (first==second) continue;
        edges.push_back({id,first,second});
    }
    // Fundamental cycles alone do not protect a theta graph: its two basis
    // loops can retain winding while the loop between them reverses. Preserve
    // the analytical local embedding inside each biconnected cyclic block.
    // A dangling spur is a bridge and therefore does not freeze a junction.
    std::map<std::string,std::vector<std::size_t>,std::less<>> adjacency;
    for (std::size_t index=0;index<edges.size();++index) {
        adjacency[edges[index].first].push_back(index);
        adjacency[edges[index].second].push_back(index);
    }
    const auto validate_block=[&](const std::vector<std::size_t>& block) {
        if (block.size()<2 || std::none_of(block.begin(),block.end(),[&](const auto index) {
            return changed_walls.contains(edges[index].id);
        })) return;
        std::map<std::string,std::vector<std::pair<std::size_t,bool>>,std::less<>> incident;
        for (const auto index : block) {
            incident[edges[index].first].emplace_back(index,true);
            incident[edges[index].second].emplace_back(index,false);
        }
        for (const auto& [vertex,branches] : incident) {
            (void)vertex;
            if (branches.size()<3) continue;
            const auto order=[&](const Entities& entities) -> std::optional<std::vector<std::string>> {
                struct Ray {
                    double angle,angle_roundoff,curvature,chord_length;
                    std::string identity;
                    Segment segment;
                };
                std::vector<Ray> rays;
                std::optional<Vec2> position;
                const auto pi=std::acos(-1.0);
                for (const auto& [index,forward] : branches) {
                    const auto segment=topology_baseline(entities.at(edges[index].id));
                    const auto endpoint=forward ? segment.start : segment.end;
                    if (position && !topology_near(*position,endpoint)) return std::nullopt;
                    if (!position) position=endpoint;
                    // Signed sweep defines endpoint tangents exactly; chord
                    // direction is not the local direction of an arc.
                    const auto chord=std::atan2(segment.end.y-segment.start.y,segment.end.x-segment.start.x);
                    auto angle=forward ? chord-segment.sweep_radians/2 : chord+segment.sweep_radians/2+pi;
                    auto angle_scale=std::abs(chord)+std::abs(segment.sweep_radians/2)+(forward ? 0 : pi);
                    if (angle<0 || angle>=2*pi) angle_scale+=2*pi;
                    angle=std::fmod(angle,2*pi); if (angle<0) angle+=2*pi;
                    const auto angle_roundoff=64*std::numeric_limits<double>::epsilon()*angle_scale;
                    const auto length=std::hypot(segment.end.x-segment.start.x,segment.end.y-segment.start.y);
                    const auto curvature=(forward ? 2 : -2)*std::sin(segment.sweep_radians/2)/length;
                    if (!std::isfinite(curvature)) invalid("Constraint wall-cycle has indeterminate analytical branch curvature");
                    rays.push_back({angle,angle_roundoff,curvature,length,topology_point({edges[index].id,
                        forward ? WallEndpointRole::start : WallEndpointRole::end}),segment});
                }
                // Exact sorts remain strict weak orders. Roundoff is applied
                // only afterwards to clusters anchored at their first value,
                // never through a non-transitive epsilon comparator.
                std::sort(rays.begin(),rays.end(),[](const auto& a,const auto& b) {
                    return a.angle!=b.angle ? a.angle<b.angle : a.identity<b.identity;
                });
                std::size_t cut=0;
                double largest_gap=-1;
                for (std::size_t i=0;i<rays.size();++i) {
                    const auto next=(i+1)%rays.size();
                    const auto gap=rays[next].angle-rays[i].angle+(next==0 ? 2*pi : 0);
                    if (gap>largest_gap) { largest_gap=gap; cut=next; }
                }
                // Put the circular seam in the largest gap so a tangent fan
                // crossing zero stays one cluster, including after rotation.
                std::rotate(rays.begin(),rays.begin()+cut,rays.end());
                const auto origin=rays.front().angle;
                for (auto& ray : rays) if (ray.angle<origin) {
                    ray.angle+=2*pi;
                    ray.angle_roundoff+=64*std::numeric_limits<double>::epsilon()*2*pi;
                }
                std::vector<std::string> result;
                for (std::size_t first=0;first<rays.size();) {
                    auto last=first+1;
                    // Relation residual tolerance is not geometric tangent
                    // equality: a tiny angle can enclose a large real area.
                    while (last<rays.size() && rays[last].angle-rays[first].angle<=
                        rays[first].angle_roundoff+rays[last].angle_roundoff) ++last;
                    // For equal tangents, the outgoing signed curvature is
                    // the next analytical term of the local circular embedding.
                    // Reversing an endpoint reverses its signed curvature.
                    std::sort(rays.begin()+first,rays.begin()+last,[](const auto& a,const auto& b) {
                        return a.curvature!=b.curvature ? a.curvature<b.curvature : a.identity<b.identity;
                    });
                    for (auto begin=first;begin<last;) {
                        auto end=begin+1;
                        const auto scale=std::max(std::abs(rays[begin].curvature),1/rays[begin].chord_length);
                        const auto roundoff=64*std::numeric_limits<double>::epsilon()*scale;
                        while (end<last && rays[end].curvature-rays[begin].curvature<=roundoff) ++end;
                        // Numerically indistinguishable tangent/curvature must
                        // describe actual overlapping local geometry before
                        // identities may break a tie. Separated rays are an
                        // indeterminate embedding, never an invented ordering.
                        for (auto i=begin;i<end;++i) for (auto j=i+1;j<end;++j)
                            if (segment_intersection(rays[i].segment,rays[j].segment,linear_tolerance).kind!=
                                SegmentIntersectionKind::overlap)
                                invalid("Constraint wall-cycle has indeterminate analytical branch topology");
                        std::sort(rays.begin()+begin,rays.begin()+end,[](const auto& a,const auto& b) {
                            return a.identity<b.identity;
                        });
                        for (auto i=begin;i<end;++i) result.push_back(rays[i].identity);
                        begin=end;
                    }
                    first=last;
                }
                const auto first=std::min_element(result.begin(),result.end());
                std::rotate(result.begin(),first,result.end());
                return result;
            };
            const auto old_order=order(before);
            if (!old_order) continue; // Do not invent a previously closed fan.
            const auto current_order=order(after);
            if (!current_order || *old_order!=*current_order)
                invalid("Constraint edit would change analytical wall-cycle branch topology");
        }
    };
    // Iterative Tarjan traversal keeps stack use bounded by the graph storage
    // even for long components, and visits every edge a constant number of times.
    std::map<std::string,std::size_t,std::less<>> discovered,low;
    std::vector<std::size_t> edge_stack;
    struct Visit { std::string vertex; std::optional<std::size_t> parent_edge; std::size_t next=0; };
    std::size_t clock=0;
    for (const auto& [start,neighbors] : adjacency) {
        (void)neighbors;
        if (discovered.contains(start)) continue;
        discovered[start]=low[start]=++clock;
        std::vector<Visit> visits{{start,std::nullopt,0}};
        while (!visits.empty()) {
            auto& visit=visits.back();
            if (visit.next<adjacency.at(visit.vertex).size()) {
                const auto index=adjacency.at(visit.vertex)[visit.next++];
                if (visit.parent_edge==index) continue;
                const auto& edge=edges[index];
                const auto next=edge.first==visit.vertex ? edge.second : edge.first;
                if (!discovered.contains(next)) {
                    edge_stack.push_back(index);
                    discovered[next]=low[next]=++clock;
                    visits.push_back({next,index,0});
                } else if (discovered.at(next)<discovered.at(visit.vertex)) {
                    edge_stack.push_back(index);
                    low[visit.vertex]=std::min(low.at(visit.vertex),discovered.at(next));
                }
                continue;
            }
            const auto finished=visit;
            visits.pop_back();
            if (!finished.parent_edge) continue;
            const auto index=*finished.parent_edge;
            const auto& edge=edges[index];
            const auto parent=edge.first==finished.vertex ? edge.second : edge.first;
            low[parent]=std::min(low.at(parent),low.at(finished.vertex));
            if (low.at(finished.vertex)>=discovered.at(parent)) {
                std::vector<std::size_t> block;
                do {
                    block.push_back(edge_stack.back()); edge_stack.pop_back();
                } while (block.back()!=index);
                validate_block(block);
            }
        }
    }
    TopologyPoints forest;
    std::map<std::string,std::vector<std::pair<std::string,std::size_t>>,std::less<>> tree;
    for (std::size_t index=0;index<edges.size();++index) {
        const auto& edge=edges[index];
        if (forest.root(edge.first)!=forest.root(edge.second)) {
            forest.join(edge.first,edge.second);
            tree[edge.first].emplace_back(edge.second,index);
            tree[edge.second].emplace_back(edge.first,index);
            continue;
        }
        // Each non-tree edge closes one deterministic fundamental cycle.
        // Branches remain in the forest and cannot suppress cycle admission.
        std::map<std::string,std::pair<std::string,std::size_t>,std::less<>> parent;
        std::vector<std::string> pending{edge.first};
        parent.emplace(edge.first,std::make_pair(edge.first,index));
        while (!pending.empty() && !parent.contains(edge.second)) {
            const auto vertex=pending.back(); pending.pop_back();
            for (const auto& [next,tree_edge] : tree.at(vertex)) {
                if (parent.try_emplace(next,std::make_pair(vertex,tree_edge)).second) pending.push_back(next);
            }
        }
        if (!parent.contains(edge.second)) invalid("Explicit wall cycle could not be reconstructed");
        std::vector<std::pair<std::string,bool>> loop{{edge.id,true}};
        bool affected=changed_walls.contains(edge.id);
        auto vertex=edge.second;
        while (vertex!=edge.first) {
            const auto& [previous,path_edge]=parent.at(vertex);
            const auto& selected=edges[path_edge];
            loop.emplace_back(selected.id,selected.first==vertex);
            affected=affected || changed_walls.contains(selected.id);
            vertex=previous;
        }
        if (affected) validate_topology_cycle(loop,before,after);
    }
    };
    for (const auto& group : groups) { validate_cycles(before,group); validate_cycles(after,group); }
}
void validate_exterior_corner_edit_topology(const Entities& before,const Entities& after) {
    const auto active_physical = [](const Entities& entities) {
        auto result = entities;
        std::set<std::string,std::less<>> unavailable;
        for (const auto& [id,entity] : entities) {
            (void)id;
            if (entity.type != "model_phases") continue;
            const auto model = ModelPhases::from_json(entity.properties.at("model"));
            const auto active = model.active_state();
            for (const auto& owner : model.entity_ids())
                if (!active.contains(owner) || active.at(owner) == ModelPhase::demolished) unavailable.insert(owner);
        }
        std::vector<std::string> active_wall_ids;
        for (const auto& [id,entity] : entities) {
            if (!topology_physical_wall(entity)) continue;
            if (unavailable.contains(id)) result.erase(id);
            else active_wall_ids.push_back(id);
        }
        for (auto& [id,placement] : resolve_vertical_placements(entities,active_wall_ids))
            result.at(id) = std::move(placement);
        return result;
    };
    const auto active_before = active_physical(before), active_after = active_physical(after);
    for (const auto& [id,entity] : before)
        if (topology_physical_wall(entity) && !active_before.contains(id) &&
            (!after.contains(id) || after.at(id) != entity))
            invalid("Exterior corner cannot edit an unavailable physical phase wall: " + id);
    std::set<std::string> phases;
    for (const auto& [id,entity] : active_after) {
        (void)id;
        if (topology_physical_wall(entity)) phases.insert(entity.properties.value("phase_id",json(nullptr)).dump());
    }
    for (const auto& phase : phases) {
        auto phase_before = active_before, phase_after = active_after;
        std::erase_if(phase_before,[&](const auto& item) {
            return topology_physical_wall(item.second) && item.second.properties.value("phase_id",json(nullptr)).dump() != phase;
        });
        std::erase_if(phase_after,[&](const auto& item) {
            return topology_physical_wall(item.second) && item.second.properties.value("phase_id",json(nullptr)).dump() != phase;
        });
        validate_constraint_edit_topology(phase_before,phase_after);
    }
}
} // namespace sketch
