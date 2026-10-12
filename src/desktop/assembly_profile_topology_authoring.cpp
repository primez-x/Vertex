#include "assembly_profile_topology_authoring.hpp"

#include <QUuid>

#include <algorithm>
#include <set>
#include <utility>

namespace sketch::desktop {
namespace {

bool same_boundary(const Boundary& first, const Boundary& second) {
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
        [](const auto& a, const auto& b) {
            return a.start.x == b.start.x && a.start.y == b.start.y &&
                a.end.x == b.end.x && a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
        });
}

bool same_profile_geometry(const AssemblyProfile& first, const AssemblyProfile& second) {
    return first.planar_affine == second.planar_affine &&
        same_boundary(first.outer, second.outer) && first.holes.size() == second.holes.size() &&
        std::equal(first.holes.begin(), first.holes.end(), second.holes.begin(), same_boundary);
}

} // namespace

bool assembly_profiles_have_topology(const std::vector<AssemblyProfile>& profiles) {
    return std::any_of(profiles.begin(), profiles.end(), [](const auto& profile) { return profile.topology.has_value(); });
}

void assign_fresh_profile_topology(AssemblyProfile& profile,
    const std::vector<AssemblyProfile>& own_profiles,
    const std::vector<AssemblyProfile>& type_profiles,
    const AssemblyProfileTopologyIdentityGenerator& identity) {
    std::set<std::string> issued;
    // Equal edge counts never authorize reusing actual source identities.
    const auto reserve_profile = [&](const std::vector<AssemblyProfile>& profiles) {
        const auto found = std::find_if(profiles.begin(), profiles.end(),
            [&](const auto& value) { return value.id == profile.id; });
        if (found == profiles.end() || !found->topology) return;
        const auto reserve = [&](const AssemblyProfileRingIdentity& ring) {
            issued.insert(ring.ring_id);
            issued.insert(ring.segment_ids.begin(), ring.segment_ids.end());
            issued.insert(ring.vertex_ids.begin(), ring.vertex_ids.end());
        };
        reserve(found->topology->outer);
        for (const auto& hole : found->topology->holes) reserve(hole);
    };
    reserve_profile(own_profiles);
    reserve_profile(type_profiles);
    const auto fresh = [&] {
        std::string id;
        do {
            id = identity ? identity() : QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        } while (!issued.insert(id).second);
        return id;
    };
    const auto ring = [&](const Boundary& boundary) {
        AssemblyProfileRingIdentity result;
        result.ring_id = fresh();
        result.segment_ids.reserve(boundary.size());
        result.vertex_ids.reserve(boundary.size());
        for (std::size_t i = 0; i < boundary.size(); ++i) {
            result.segment_ids.push_back(fresh());
            result.vertex_ids.push_back(fresh());
        }
        return result;
    };
    AssemblyProfileTopology result;
    result.outer = ring(profile.outer);
    result.holes.reserve(profile.holes.size());
    for (const auto& hole : profile.holes) result.holes.push_back(ring(hole));
    profile.topology = std::move(result);
    validate_assembly_profile_topology(profile);
}

void collect_reviewed_profile_topology(std::vector<AssemblyProfile>& profiles,
    const std::vector<AssemblyProfile>& own_profiles,
    const std::vector<AssemblyProfile>& type_profiles,
    const AssemblyProfileTopologyIdentityGenerator& identity) {
    for (auto& profile : profiles) {
        const auto actual = std::find_if(own_profiles.begin(), own_profiles.end(),
            [&](const auto& value) { return value.id == profile.id; });
        if (actual != own_profiles.end() && same_profile_geometry(*actual, profile))
            profile.topology = actual->topology;
        else
            assign_fresh_profile_topology(profile, own_profiles, type_profiles, identity);
    }
}

} // namespace sketch::desktop
