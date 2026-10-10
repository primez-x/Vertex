#include "sketch/roof_opening_group_edit.hpp"

#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t group_limit = 4096;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void finite(Vec2 value) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y))
        invalid("Skylight group coordinates must be finite");
}
void bounded_group(std::size_t size) {
    if (size == 0 || size > group_limit) invalid("Skylight group must contain between one and 4096 members");
}
const Entity& actual_roof(const Entities& actual, const std::string& id) {
    const auto found = actual.find(id);
    if (found == actual.end() || found->first != found->second.id || found->second.type != "roof")
        invalid("Skylight group roof is missing or inconsistent");
    return found->second;
}

struct Host {
    const Entity* entity;
    RoofObject object;
    Vec2 base;
    double yaw;
};
Host host(const Entity& entity) {
    auto object = decode_roof_entity(entity);
    const auto [base, yaw] = std::visit([](const auto& roof) {
        return std::pair{Vec2{roof.base_position.x, roof.base_position.y}, roof.orientation_radians};
    }, object);
    return {&entity, std::move(object), base, yaw};
}
using Hosts = std::map<std::string, Host, std::less<>>;
const Host& actual_host(const Entities& actual, Hosts& hosts, const std::string& id) {
    const auto found = hosts.find(id);
    if (found != hosts.end()) return found->second;
    return hosts.emplace(id, host(actual_roof(actual, id))).first->second;
}
RoofOpening skylight(const Host& owner, const std::string& id) {
    return std::visit([&](const auto& roof) {
        std::set<std::string, std::less<>> identities;
        const RoofOpening* selected = nullptr;
        for (const auto& opening : roof.openings) {
            if (!identities.insert(opening.id).second) invalid("Skylight source has duplicate child identities");
            if (opening.id == id) selected = &opening;
        }
        if (!selected || !selected->skylight) invalid("Skylight group requires an actual profiled source child");
        return *selected;
    }, owner.object);
}
Vec2 center(const RoofOpening& opening) {
    const Vec2 result{opening.x + opening.width * .5, opening.y + opening.depth * .5};
    finite(result);
    return result;
}
Vec2 world(const Host& owner, Vec2 local) {
    if (owner.yaw == 0.0) {
        const Vec2 result{owner.base.x + local.x, owner.base.y + local.y};
        finite(result); return result;
    }
    const auto c = std::cos(owner.yaw), s = std::sin(owner.yaw);
    const Vec2 result{owner.base.x + c * local.x - s * local.y,
        owner.base.y + s * local.x + c * local.y};
    finite(result); return result;
}
Vec2 local_delta(const Host& owner, Vec2 delta) {
    finite(delta);
    if (owner.yaw == 0.0) return delta;
    const auto c = std::cos(owner.yaw), s = std::sin(owner.yaw);
    const Vec2 result{c * delta.x + s * delta.y, -s * delta.x + c * delta.y};
    finite(result); return result;
}
Vec2 local(const Host& owner, Vec2 point) {
    return local_delta(owner, {point.x - owner.base.x, point.y - owner.base.y});
}
Vec2 scales(const Host& owner, Vec2 point) {
    return std::visit([&](const auto& roof) { return roof_opening_reference_surface_scales(roof, point); }, owner.object);
}
RoofOpeningPlanFrame plan_frame(const Host& owner, const RoofOpening& opening) {
    return std::visit([&](const auto& roof) { return roof_opening_plan_frame(roof, opening); }, owner.object);
}
Vec2 world_direction(const Host& owner, Vec2 direction) {
    if (owner.yaw == 0.0) return direction;
    const auto c = std::cos(owner.yaw), s = std::sin(owner.yaw);
    const Vec2 result{c * direction.x - s * direction.y, s * direction.x + c * direction.y};
    finite(result); return result;
}
struct AxisResize {
    Vec2 scale;
    double c;
    double s;

    Vec2 apply(Vec2 value) const {
        if (c == 1.0 && s == 0.0) {
            const Vec2 result{scale.x * value.x, scale.y * value.y};
            finite(result); return result;
        }
        const Vec2 aligned{scale.x * (c * value.x + s * value.y),
            scale.y * (-s * value.x + c * value.y)};
        const Vec2 result{c * aligned.x - s * aligned.y, s * aligned.x + c * aligned.y};
        finite(result); return result;
    }

    double dimension_factor(Vec2 direction, double uniform_scale) const {
        finite(direction);
        const auto length = std::hypot(direction.x, direction.y);
        if (!std::isfinite(length) || length <= 0.0)
            invalid("Skylight source projected direction must have positive finite length");
        // A pitched facet's projected directions need not be unit length or
        // orthogonal. Normalize each independently before measuring |A p|/|p|.
        const auto resized = apply({direction.x / length, direction.y / length});
        const auto factor = uniform_scale * std::hypot(resized.x, resized.y);
        if (!std::isfinite(factor) || factor <= 0.0)
            invalid("Skylight group dimension factors must be positive and finite");
        return factor;
    }
};
RoofOpeningQuantityInput quantity(double metres) {
    if (!std::isfinite(metres)) invalid("Skylight group dimensions must be finite");
    std::array<char, 64> buffer{};
    for (int digits = 17; digits > 0; --digits) {
        const auto spelling = std::to_chars(buffer.data(), buffer.data() + buffer.size(), metres,
            std::chars_format::general, digits);
        if (spelling.ec != std::errc{}) continue;
        try {
            auto parsed = parse_quantity(std::string(buffer.data(), spelling.ptr) + " m", Unit::metre);
            if (parsed.metres == metres) return {std::move(parsed), Unit::metre};
        } catch (const std::invalid_argument&) {} catch (const std::overflow_error&) {}
    }
    invalid("Skylight group position has no exact supported measurement");
}
Vec2 rebased_size(const RoofOpening& source, Vec2 before, Vec2 after, Vec2 scale) {
    const Vec2 result{before.x == after.x ? source.width * scale.x : source.width * scale.x * (before.x / after.x),
        before.y == after.y ? source.depth * scale.y : source.depth * scale.y * (before.y / after.y)};
    finite(result);
    if (result.x <= 0.0 || result.y <= 0.0) invalid("Skylight group dimensions must remain positive");
    return result;
}
Vec2 rebased_size(const RoofOpening& source, Vec2 before, Vec2 after, double scale) {
    return rebased_size(source, before, after, Vec2{scale, scale});
}
RoofOpeningEditIntent opening_intent(const Host& owner) {
    RoofOpeningEditIntent result;
    result.roof_id = owner.entity->id;
    result.uses_skylight_schema = true;
    result.uses_rotation_schema = owner.entity->properties.at("version") == 4;
    return result;
}
RoofEditIntent composite(RoofOpeningEditIntent opening) {
    RoofEditIntent result;
    result.roof_id = opening.roof_id;
    result.openings = std::move(opening);
    return result;
}
Vec2 centroid(const std::vector<Vec2>& points) {
    bounded_group(points.size());
    // Divide before summing, avoiding overflow for a finite representable mean.
    long double x = 0.0L, y = 0.0L;
    const auto count = static_cast<long double>(points.size());
    for (const auto point : points) {
        finite(point);
        x += static_cast<long double>(point.x) / count;
        y += static_cast<long double>(point.y) / count;
    }
    const Vec2 result{static_cast<double>(x), static_cast<double>(y)};
    finite(result); return result;
}
using MemberIds = std::set<std::pair<std::string, std::string>>;
void unique(MemberIds& ids, const std::string& roof_id, const std::string& opening_id) {
    if (!ids.emplace(roof_id, opening_id).second) invalid("Skylight group contains a duplicate source child");
}
void consistent_passive_source(Hosts& hosts, const RoofOpeningCloneSource& source) {
    const auto found = hosts.find(source.roof.id);
    if (found == hosts.end()) { hosts.emplace(source.roof.id, host(source.roof)); return; }
    const auto& previous = *found->second.entity;
    if (previous != source.roof || previous.properties.dump() != source.roof.properties.dump() ||
        previous.extensions.dump() != source.roof.extensions.dump())
        invalid("Skylight group contains conflicting passive source roofs");
}
// Fresh physical replay must not borrow actual-map authority. Inspect actual
// names separately, including opaque keys/strings, without adding prepared
// roofs to that map or treating them as captured document owners.
struct FreshActualReservation {
    const std::set<std::string, std::less<>>& fresh;
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        constexpr std::size_t byte_limit = 64 * 1024 * 1024;
        if (value.size() > byte_limit - bytes) invalid("Skylight actual identity byte budget exceeded");
        bytes += value.size();
        if (fresh.contains(value)) invalid("Fresh skylight identity aliases actual source data");
    }
    void read(const nlohmann::json& value, unsigned depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024)
            invalid("Skylight actual identity complexity budget exceeded");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
    void entity(const Entity& value) {
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
};
} // namespace

Vec2 roof_opening_group_member_center_world(const Entities& actual, const RoofOpeningGroupMember& member) {
    const auto owner = host(actual_roof(actual, member.roof_id));
    return world(owner, center(skylight(owner, member.opening_id)));
}
Vec2 roof_opening_group_anchor_world(const Entities& actual, const std::vector<RoofOpeningGroupMember>& members) {
    bounded_group(members.size());
    Hosts hosts;
    MemberIds ids;
    std::vector<Vec2> points;
    points.reserve(members.size());
    for (const auto& member : members) {
        unique(ids, member.roof_id, member.opening_id);
        const auto& owner = actual_host(actual, hosts, member.roof_id);
        points.push_back(world(owner, center(skylight(owner, member.opening_id))));
    }
    return centroid(points);
}
Vec2 roof_opening_group_clone_anchor_world(const std::vector<RoofOpeningGroupClone>& clones) {
    bounded_group(clones.size());
    Hosts hosts;
    MemberIds ids;
    std::vector<Vec2> points;
    points.reserve(clones.size());
    for (const auto& clone : clones) {
        unique(ids, clone.source.roof.id, clone.source.opening_id);
        consistent_passive_source(hosts, clone.source);
        const auto& owner = hosts.at(clone.source.roof.id);
        points.push_back(world(owner, center(skylight(owner, clone.source.opening_id))));
    }
    return centroid(points);
}

std::vector<RoofEditIntent> prepare_roof_opening_group_transform(const Entities& actual,
    const RoofOpeningGroupTransform& request) {
    bounded_group(request.members.size());
    finite(request.world_pivot); finite(request.world_translation);
    if (!std::isfinite(request.rotation_radians) || !std::isfinite(request.uniform_scale) || request.uniform_scale <= 0.0)
        invalid("Skylight group transform requires a finite angle and positive finite scale");
    finite(request.axis_scale);
    if (request.axis_scale.x <= 0.0 || request.axis_scale.y <= 0.0 ||
        !std::isfinite(request.axis_rotation_radians))
        invalid("Skylight group axis resize requires positive finite scales and a finite angle");
    const bool directional = request.axis_scale.x != request.axis_scale.y;
    // Identity axes retain the old arithmetic exactly, including its no-op and
    // pure-translation paths. Equal axes are the same uniform operation.
    const auto uniform_scale = !directional && request.axis_scale.x != 1.0
        ? request.uniform_scale * request.axis_scale.x : request.uniform_scale;
    if (!std::isfinite(uniform_scale) || uniform_scale <= 0.0)
        invalid("Skylight group combined scale must be positive and finite");
    const auto axis_angle = directional ? std::remainder(request.axis_rotation_radians, 2.0 * std::numbers::pi) : 0.0;
    const AxisResize axes{request.axis_scale, axis_angle == 0.0 ? 1.0 : std::cos(axis_angle),
        axis_angle == 0.0 ? 0.0 : std::sin(axis_angle)};
    const auto angle = std::remainder(request.rotation_radians, 2.0 * std::numbers::pi);
    const bool translation_only = !directional && angle == 0.0 && uniform_scale == 1.0;
    const bool identity = translation_only &&
        request.world_translation.x == 0.0 && request.world_translation.y == 0.0;
    const auto c = angle == 0.0 ? 1.0 : std::cos(angle), s = angle == 0.0 ? 0.0 : std::sin(angle);
    Hosts hosts;
    MemberIds ids;
    std::map<std::string, RoofOpeningEditIntent, std::less<>> grouped;
    for (const auto& member : request.members) {
        unique(ids, member.roof_id, member.opening_id);
        const auto& owner = actual_host(actual, hosts, member.roof_id);
        const auto source = skylight(owner, member.opening_id);
        auto [entry, inserted] = grouped.try_emplace(member.roof_id, opening_intent(owner));
        (void)inserted;
        RoofOpeningUpsertIntent row; row.opening_id = member.opening_id;
        if (!identity) {
            const auto old_center = center(source), old_world = world(owner, old_center);
            Vec2 new_world;
            if (translation_only) {
                new_world = {old_world.x + request.world_translation.x, old_world.y + request.world_translation.y};
            } else {
                Vec2 delta{old_world.x - request.world_pivot.x, old_world.y - request.world_pivot.y};
                finite(delta);
                if (directional) delta = axes.apply(delta);
                new_world = {request.world_pivot.x + uniform_scale * (c * delta.x - s * delta.y) + request.world_translation.x,
                    request.world_pivot.y + uniform_scale * (s * delta.x + c * delta.y) + request.world_translation.y};
            }
            finite(new_world);
            // Apply displacement to the actual local centre. Inverting an
            // absolute world position would introduce base/yaw roundoff even
            // in a pure translation's otherwise untouched local coordinate.
            const auto displacement = translation_only
                ? request.world_translation : Vec2{new_world.x - old_world.x, new_world.y - old_world.y};
            const auto delta = local_delta(owner, displacement);
            const Vec2 new_center{old_center.x + delta.x, old_center.y + delta.y};
            finite(new_center);
            const auto size = [&] {
                if (!directional)
                    return rebased_size(source, scales(owner, old_center), scales(owner, new_center), uniform_scale);
                const auto frame = plan_frame(owner, source);
                const Vec2 factors{axes.dimension_factor(world_direction(owner, frame.along), uniform_scale),
                    axes.dimension_factor(world_direction(owner, frame.across), uniform_scale)};
                return rebased_size(source, {frame.width_surface_scale, frame.depth_surface_scale},
                    scales(owner, new_center), factors);
            }();
            const auto x = source.x + (new_center.x - old_center.x) - (size.x - source.width) * .5;
            const auto y = source.y + (new_center.y - old_center.y) - (size.y - source.depth) * .5;
            if (x != source.x) row.x = quantity(x);
            if (y != source.y) row.y = quantity(y);
            if (size.x != source.width) row.width = quantity(size.x);
            if (size.y != source.depth) row.depth = quantity(size.y);
            const auto rotation = angle == 0.0 ? source.rotation_radians :
                std::remainder(source.rotation_radians + angle, 2.0 * std::numbers::pi);
            if (rotation != source.rotation_radians) {
                row.rotation_radians = rotation;
                entry->second.uses_rotation_schema = true;
            }
        }
        entry->second.upserts.push_back(std::move(row));
    }
    std::vector<RoofEditIntent> result;
    result.reserve(grouped.size());
    for (auto& [id, opening] : grouped) result.push_back(composite(std::move(opening)));
    const auto admitted = replay_roof_edit_entities(actual, result);
    // Retained-only intents validate every requested source, including identity
    // operations. They never become authored edits or promote an unchanged roof.
    std::erase_if(result, [&](const auto& edit) { return admitted.at(edit.roof_id) == actual.at(edit.roof_id); });
    return result;
}

std::vector<RoofEditIntent> prepare_roof_opening_group_removal(const Entities& actual,
    const std::vector<RoofOpeningGroupMember>& members) {
    bounded_group(members.size());
    Hosts hosts;
    std::set<std::pair<std::string,std::string>> selected;
    std::map<std::string,RoofOpeningEditIntent,std::less<>> grouped;
    for (const auto& member:members) {
        if (!selected.emplace(member.roof_id,member.opening_id).second)
            invalid("Skylight removal contains a duplicate source child");
        const auto& owner=actual_host(actual,hosts,member.roof_id);
        (void)skylight(owner,member.opening_id);
        auto [entry,inserted]=grouped.try_emplace(member.roof_id,opening_intent(owner));
        (void)inserted;
        entry->second.removed_opening_ids.push_back(member.opening_id);
    }
    std::vector<RoofEditIntent> result;
    result.reserve(grouped.size());
    for (auto& [id,intent]:grouped) {
        (void)id;
        std::sort(intent.removed_opening_ids.begin(),intent.removed_opening_ids.end());
        auto edit=composite(std::move(intent));
        edit.coordinate_world_hosted_geometry=true;
        result.push_back(std::move(edit));
    }
    (void)replay_roof_edit_entities(actual,result);
    return result;
}

RoofEditIntent prepare_roof_opening_group_clone_placement(const Entities& actual,
    const RoofOpeningGroupClonePlacement& request, const Entities& fresh_roofs) {
    bounded_group(request.clones.size());
    finite(request.destination_anchor_world);
    if (fresh_roofs.size() > group_limit) invalid("Skylight fresh roof host budget exceeded");
    for (const auto& [id, roof] : fresh_roofs) {
        if (id != roof.id || roof.type != "roof" || actual.contains(id))
            invalid("Skylight fresh roof host overlaps actual content or has an inconsistent role");
    }
    const bool fresh_destination = fresh_roofs.contains(request.destination_roof_id);
    const auto destination = host(actual_roof(fresh_destination ? fresh_roofs : actual, request.destination_roof_id));
    const auto anchor = roof_opening_group_clone_anchor_world(request.clones);
    auto opening = opening_intent(destination);
    opening.uses_clone_schema = true;
    Hosts sources;
    for (const auto& clone : request.clones) {
        consistent_passive_source(sources, clone.source);
        const auto& owner = sources.at(clone.source.roof.id);
        const auto source = skylight(owner, clone.source.opening_id);
        const auto source_center = center(source), source_world = world(owner, source_center);
        const Vec2 destination_world{request.destination_anchor_world.x + (source_world.x - anchor.x),
            request.destination_anchor_world.y + (source_world.y - anchor.y)};
        finite(destination_world);
        const bool same_frame = destination.base.x == owner.base.x && destination.base.y == owner.base.y &&
            destination.yaw == owner.yaw;
        const auto destination_center = same_frame && destination_world.x == source_world.x && destination_world.y == source_world.y
            ? source_center : local(destination, destination_world);
        const auto size = rebased_size(source, scales(owner, source_center), scales(destination, destination_center), 1.0);
        RoofOpeningUpsertIntent row;
        row.opening_id = clone.opening_id;
        row.clone_source = clone.source;
        row.x = quantity(same_frame ? source.x + (destination_center.x - source_center.x) - (size.x - source.width) * .5
            : destination_center.x - size.x * .5);
        row.y = quantity(same_frame ? source.y + (destination_center.y - source_center.y) - (size.y - source.depth) * .5
            : destination_center.y - size.y * .5);
        row.width = quantity(size.x); row.depth = quantity(size.y);
        for (const auto& raw : clone.source.roof.properties.at("roof_openings"))
            if (raw.at("id") == clone.source.opening_id) row.skylight = raw.at("skylight");
        // Explicitly retain schema-four rotation, including zero. Historical
        // opaque angles on older schemas remain passive and are never activated.
        if (clone.source.roof.properties.at("version") == 4) {
            row.rotation_radians = source.rotation_radians;
            opening.uses_rotation_schema = true;
        }
        opening.upserts.push_back(std::move(row));
    }
    auto result = composite(std::move(opening));
    if (fresh_destination) {
        // This explicit prepared-host inventory is not an actual source map.
        // Reuse its physical child/opaque-name freshness checks, then inspect
        // actual names independently. The caller reserves retained history and
        // all other family names before entering this preparation leaf.
        const auto children = new_roof_opening_identity_ids(fresh_roofs, {*result.openings});
        std::set<std::string, std::less<>> fresh(children.begin(), children.end());
        for (const auto& [id, roof] : fresh_roofs) { (void)roof; fresh.insert(id); }
        FreshActualReservation reservation{fresh};
        for (const auto& [id, entity] : actual) { reservation.text(id); reservation.entity(entity); }
        (void)replay_roof_edit_entity(*destination.entity, result);
    } else (void)replay_roof_edit_entities(actual, {result});
    return result;
}

} // namespace sketch
