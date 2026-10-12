#include "sketch/assembly_directional_resize.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

using Exact = boost::multiprecision::cpp_rational;
using Integer = boost::multiprecision::cpp_int;

// Fixed expressions on binary64 input, with bounded exponent/integer work.
// Windows long double need not be wider than double. Retain differences and
// products exactly until the one final binary64 rounding of each map field.
Exact binary64(double value) {
    static_assert(std::numeric_limits<double>::is_iec559 &&
        std::numeric_limits<double>::digits == 53 && sizeof(double) == sizeof(std::uint64_t));
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
    require(exponent != 0x7ff, "directional resize arithmetic must be finite");
    Integer significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (exponent) significand += Integer{1} << 52;
    if (bits >> 63) significand = -significand;
    const int shift = exponent ? static_cast<int>(exponent) - 1075 : -1074;
    if (shift >= 0) return Exact{significand << shift};
    Exact result{significand}; result /= Integer{1} << -shift; return result;
}

double rounded(const Exact& value) {
    if (value == 0) return 0.0;
    const bool negative = value < 0;
    const Exact magnitude = negative ? -value : value;
    require(magnitude <= binary64(std::numeric_limits<double>::max()),
        "directional resize arithmetic exceeds numeric range");
    const Integer num = numerator(magnitude), den = denominator(magnitude);
    int exponent = static_cast<int>(boost::multiprecision::msb(num)) -
        static_cast<int>(boost::multiprecision::msb(den));
    if (exponent >= 0) { if (num < (den << exponent)) --exponent; }
    else if ((num << -exponent) < den) --exponent;
    const int quantum = std::max(exponent - 52, -1074);
    const Integer scaled_num = quantum < 0 ? Integer{num << -quantum} : num;
    const Integer scaled_den = quantum > 0 ? Integer{den << quantum} : den;
    Integer significand = scaled_num / scaled_den;
    const Integer remainder = scaled_num % scaled_den;
    if ((remainder << 1) > scaled_den ||
        ((remainder << 1) == scaled_den && (significand & 1) != 0)) ++significand;
    const double result = std::ldexp(significand.convert_to<double>(), quantum);
    require(std::isfinite(result) && result != 0.0,
        "directional resize arithmetic overflows/underflows");
    return negative ? -result : result;
}

using Path = std::vector<std::string>;
using NodeIndex = std::map<Path, const AssemblyExpandedNode*>;

void budget_valid(const AssemblyExpansionBudget& budget) {
    require(budget.max_nodes <= 4096 && budget.max_profile_segments <= 262144 &&
        budget.max_mapped_profile_work <= 67108864 &&
        budget.consumed_nodes <= budget.max_nodes &&
        budget.consumed_profile_segments <= budget.max_profile_segments &&
        budget.consumed_mapped_profile_work <= budget.max_mapped_profile_work,
        "invalid directional resize work budget");
}

struct SourceIndex {
    NodeIndex nodes;
    std::size_t segments{};
    const AssemblyExpandedNode* root{};
};

SourceIndex index_source(const AssemblyExpansion& actual, const AssemblyExpansionBudget& work) {
    budget_valid(work);
    require(!actual.nodes.empty() && actual.nodes.size() <= work.max_nodes - work.consumed_nodes,
        "directional resize node budget exceeded");
    // Bound borrowed raw arrays before copying profiles, paths or topology.
    require(actual.profiles.size() <= 262144, "directional resize profile budget exceeded");
    std::size_t segments = 0;
    for (const auto& profile : actual.profiles) {
        const auto& source = profile.profile;
        require(!source.outer.empty() && source.outer.size() <= 1024 && source.holes.size() <= 1024,
            "invalid directional resize source profile size");
        auto count = source.outer.size();
        for (const auto& hole : source.holes) {
            require(!hole.empty() && hole.size() <= 1024 - count,
                "directional resize profile segment budget exceeded");
            count += hole.size();
        }
        require(count <= work.max_profile_segments - work.consumed_profile_segments - segments,
            "directional resize segment budget exceeded");
        segments += count;
    }
    SourceIndex result; result.segments = segments;
    for (const auto& node : actual.nodes) {
        require(node.part_path.size() < 32 && !node.type_id.empty(),
            "invalid directional resize actual node");
        for (const auto& id : node.part_path)
            require(!id.empty(), "invalid directional resize actual part path");
        (void)transform_assembly_point({}, node.transform);
        require(result.nodes.emplace(node.part_path, &node).second,
            "duplicate directional resize actual node");
        if (node.part_path.empty()) result.root = &node;
    }
    require(result.root && result.root->type_id == actual.source_instance.type_id,
        "directional resize actual root is missing/inconsistent");
    std::map<Path, std::set<std::string_view>> ids;
    for (const auto& profile : actual.profiles) {
        const auto found = result.nodes.find(profile.part_path);
        require(found != result.nodes.end() && found->second->type_id == profile.type_id &&
            found->second->transform == profile.transform,
            "directional resize actual own profile frame is inconsistent");
        require(ids[profile.part_path].insert(profile.profile.id).second,
            "duplicate directional resize own profile identity");
        validate_assembly_profile_topology(profile.profile);
    }
    return result;
}

void charge_linear(AssemblyExpansionBudget& work, std::size_t segments) {
    // Fixed-size exact map expressions and analytical bounds per source edge.
    constexpr std::size_t edge_work = 64;
    require(segments <= (work.max_mapped_profile_work - work.consumed_mapped_profile_work) / edge_work,
        "directional resize affine work budget exceeded");
    work.consumed_mapped_profile_work += segments * edge_work;
}

void charge_frame_coefficients(AssemblyExpansionBudget& work, std::size_t profiles) {
    // One exact frame/axis product plus the added exact relative basis and
    // anchor-delta expressions per mapped profile. Fixed-size expressions on
    // binary64 inputs remain bounded even for distant origins/large angles.
    constexpr std::size_t setup_work = 32, profile_work = 32;
    const auto remaining = work.max_mapped_profile_work - work.consumed_mapped_profile_work;
    require(remaining >= setup_work && profiles <= (remaining - setup_work) / profile_work,
        "directional resize operation frame work budget exceeded");
    work.consumed_mapped_profile_work += setup_work + profiles * profile_work;
}

struct ExactOperationFrame {
    Exact cosine, sine;
    Exact axis_cosine, axis_sine;
};

AssemblyProfilePlanarAffine inner_map(const AssemblyProfile& profile) {
    if (profile.planar_affine) return *profile.planar_affine;
    const auto anchor = profile.outer.front().start;
    return {anchor, anchor};
}

AssemblyProfile resized_profile(const AssemblyProfile& source, const AssemblyTransform& node,
    AssemblyRotationComponents axes, const AssemblyDirectionalResize& operation,
    const ExactOperationFrame* frame) {
    const auto [nc, ns] = assembly_rotation_components(node.rotation_radians);
    const auto [rc, rs] = axes;
    // Use the actual admitted rotation components rather than reducing and
    // subtracting huge authored angles (which can lose their actual yaw).
    const bool aligned = nc == rc && ns == rs;
    Exact c{1}, s{0};
    if (frame) {
        // The actual node inverse contributes its squared basis norm as a
        // common divisor, which cancels in B*diag(sx,sy)*B^-1 below.
        c = binary64(nc) * frame->axis_cosine + binary64(ns) * frame->axis_sine;
        s = -binary64(ns) * frame->axis_cosine + binary64(nc) * frame->axis_sine;
    } else if (!aligned) {
        c = binary64(nc) * binary64(rc) + binary64(ns) * binary64(rs);
        s = -binary64(ns) * binary64(rc) + binary64(nc) * binary64(rs);
    }
    const auto parity = node.mirrored_y ? -1.0 : 1.0;
    const auto sx = binary64(operation.x_factor), sy = binary64(operation.y_factor);
    const Exact relative_norm = c * c + s * s;
    require(relative_norm > 0, "directional resize relative basis is singular");
    // B*diag(sx,sy)*B^-1 in the actual node basis. Actual binary64 cosine/sine
    // pairs need not have unit squared norm. Frame Y reflection cancels around
    // the diagonal factors; node reflection changes both off-diagonal signs.
    // The relative basis's common scalar cancels in this conjugation.
    // Uniform factors are exact.
    AssemblyProfilePlanarAffine outer;
    if (operation.x_factor == operation.y_factor) outer.xx = outer.yy = operation.x_factor;
    else {
        outer.xx = rounded((sx * c * c + sy * s * s) / relative_norm);
        outer.yy = rounded((sx * s * s + sy * c * c) / relative_norm);
        outer.xy = outer.yx = rounded(binary64(parity) * (sx - sy) * c * s / relative_norm);
    }
    const auto inner = inner_map(source);
    outer.source_origin = inner.target_origin;
    // b=O^-1(a-t)/scale is retained EXACTLY, without requiring a rounded huge
    // local anchor or manufacturing/subtracting two absolute world points.
    const Exact node_norm = binary64(nc) * binary64(nc) + binary64(ns) * binary64(ns);
    require(node_norm > 0, "directional resize node basis is singular");
    Exact dx, dy;
    if (frame) {
        const auto& origin = operation.operation_frame->translation_m;
        const Exact ax = binary64(operation.world_anchor_m.x), ay = binary64(operation.world_anchor_m.y);
        // Keep the small frame-to-node difference before applying the local
        // anchor. No binary64 world anchor or summed frame/axis angle exists.
        dx = (binary64(origin.x) - binary64(node.translation_m.x)) + frame->cosine * ax - frame->sine * ay;
        dy = (binary64(origin.y) - binary64(node.translation_m.y)) + frame->sine * ax + frame->cosine * ay;
    } else {
        dx = binary64(operation.world_anchor_m.x) - binary64(node.translation_m.x);
        dy = binary64(operation.world_anchor_m.y) - binary64(node.translation_m.y);
    }
    const Exact denominator = binary64(node.scale) * node_norm;
    const Exact bx = (binary64(nc) * dx + binary64(ns) * dy) / denominator;
    const Exact by = binary64(parity) * (-binary64(ns) * dx + binary64(nc) * dy) / denominator;
    const Exact px = binary64(outer.source_origin.x), py = binary64(outer.source_origin.y);
    outer.target_origin = {rounded(px + (binary64(outer.xx) - 1) * (px - bx) + binary64(outer.xy) * (py - by)),
        rounded(py + binary64(outer.yx) * (px - bx) + (binary64(outer.yy) - 1) * (py - by))};
    validate_assembly_profile_affine(outer);
    auto result = source;
    result.planar_affine = compose_assembly_profile_affine(outer, inner);
    return result;
}

AssemblyProfilePlanarAffine oriented_map(const AssemblyExpandedProfile& profile,
    AssemblyRotationComponents axes, bool axes_mirrored_y, Vec2 origin, bool& reflect_y) {
    const auto [rc, rs] = axes;
    const auto [nc, ns] = assembly_rotation_components(profile.transform.rotation_radians);
    const auto node_parity = profile.transform.mirrored_y ? -1.0 : 1.0;
    const auto axes_parity = axes_mirrored_y ? -1.0 : 1.0;
    reflect_y = node_parity != axes_parity;
    const auto scale = binary64(profile.transform.scale);
    const Exact axes_norm = binary64(rc) * binary64(rc) + binary64(rs) * binary64(rs);
    require(axes_norm > 0, "assembly bounds axis basis is singular");
    const Exact cosine = (binary64(rc) * binary64(nc) + binary64(rs) * binary64(ns)) / axes_norm;
    const Exact sine = (-binary64(rs) * binary64(nc) + binary64(rc) * binary64(ns)) / axes_norm;
    // Remove an output Y reflection to give the affine helper a positive
    // determinant, then reflect its completed analytical bounds afterwards.
    const Exact xx = scale * cosine, xy = -scale * binary64(node_parity) * sine;
    const Exact yx = -xy, yy = xx;
    const auto inner = inner_map(profile.profile);
    validate_assembly_profile_affine(inner);
    const Exact dx = binary64(profile.transform.translation_m.x) - binary64(origin.x);
    const Exact dy = binary64(profile.transform.translation_m.y) - binary64(origin.y);
    const Exact tx = (binary64(rc) * dx + binary64(rs) * dy) / axes_norm;
    const Exact ty = binary64(node_parity) * (-binary64(rs) * dx + binary64(rc) * dy) / axes_norm;
    AssemblyProfilePlanarAffine result;
    result.source_origin = inner.source_origin;
    result.target_origin = {rounded(tx + xx * binary64(inner.target_origin.x) + xy * binary64(inner.target_origin.y)),
        rounded(ty + yx * binary64(inner.target_origin.x) + yy * binary64(inner.target_origin.y))};
    // Compose before rounding: a huge node scale and small existing map can
    // have a representable effective determinant even when scale^2 cannot.
    result.xx = rounded(xx * binary64(inner.xx) + xy * binary64(inner.yx));
    result.xy = rounded(xx * binary64(inner.xy) + xy * binary64(inner.yy));
    result.yx = rounded(yx * binary64(inner.xx) + yy * binary64(inner.yx));
    result.yy = rounded(yx * binary64(inner.xy) + yy * binary64(inner.yy));
    validate_assembly_profile_affine(result);
    return result;
}

Bounds2 profile_bounds(const AssemblyExpansion& actual, const SourceIndex& source,
    Vec2 world_origin_m, AssemblyRotationComponents axes, bool axes_mirrored_y,
    AssemblyExpansionBudget& available_work) {
    require(!actual.profiles.empty(), "assembly has no actual solid profile bounds");
    auto pending = available_work; charge_linear(pending, source.segments);
    pending.consumed_nodes += actual.nodes.size();
    pending.consumed_profile_segments += source.segments;
    std::optional<Bounds2> result;
    for (const auto& profile : actual.profiles) {
        bool reflected{};
        const auto map = oriented_map(profile, axes, axes_mirrored_y, world_origin_m, reflected);
        const auto include = [&](const Boundary& ring) {
            auto bounds = assembly_profile_boundary_bounds(ring, map);
            if (reflected) {
                const auto minimum = bounds.minimum.y;
                bounds.minimum.y = -bounds.maximum.y; bounds.maximum.y = -minimum;
            }
            if (!result) result = bounds;
            else {
                result->minimum.x = std::min(result->minimum.x, bounds.minimum.x);
                result->minimum.y = std::min(result->minimum.y, bounds.minimum.y);
                result->maximum.x = std::max(result->maximum.x, bounds.maximum.x);
                result->maximum.y = std::max(result->maximum.y, bounds.maximum.y);
            }
        };
        include(profile.profile.outer);
        for (const auto& hole : profile.profile.holes) include(hole);
    }
    available_work = std::move(pending);
    return *result;
}

} // namespace

AssemblyDirectionalResizeDraft draft_assembly_directional_resize(const AssemblyModel& model,
    const AssemblyExpansion& actual, const AssemblyDirectionalResize& operation,
    AssemblyExpansionBudget& available_work) {
    require(std::isfinite(operation.world_anchor_m.x) && std::isfinite(operation.world_anchor_m.y) &&
        std::isfinite(operation.x_factor) && operation.x_factor > 0 &&
        std::isfinite(operation.y_factor) && operation.y_factor > 0,
        "directional resize anchor/factors must be finite and positive");
    require(!operation.world_axis_rotation_radians || std::isfinite(*operation.world_axis_rotation_radians),
        "directional resize world axis rotation must be finite");
    if (operation.operation_frame) {
        const auto& frame = *operation.operation_frame;
        (void)transform_assembly_point({}, frame);
        require(frame.scale == 1.0 && frame.vertical_scale == 1.0 && !frame.mirrored_y,
            "directional resize operation frame must be orientation-preserving and rigid");
        require(operation.world_axis_rotation_radians.has_value(),
            "directional resize operation frame requires explicit local axes");
    }
    const auto source = index_source(actual, available_work);
    // Establish own-source provenance against the selected model before any
    // profile copy. Inherited profiles must not be replaced by a stale or
    // filtered presentation roster merely because it expands successfully.
    std::map<std::string_view, const AssemblyType*> types;
    for (const auto& type : model.types()) types.emplace(type.id, &type);
    std::map<Path, const AssemblyPathOverride*> overrides;
    for (const auto& row : actual.source_instance.nested_overrides)
        require(overrides.emplace(row.part_path, &row).second,
            "duplicate directional resize source path override");
    std::map<Path, const std::vector<AssemblyProfile>*> own_sources;
    for (const auto& node : actual.nodes) {
        const auto type = types.find(node.type_id);
        require(type != types.end(), "directional resize source type is missing");
        const auto* profiles = &type->second->profiles;
        if (node.part_path.empty()) {
            if (actual.source_instance.profile_overrides) profiles = &*actual.source_instance.profile_overrides;
        } else {
            const auto found = overrides.find(node.part_path);
            require((found == overrides.end() && !node.path_override) ||
                (found != overrides.end() && node.path_override && *found->second == *node.path_override),
                "directional resize source override provenance is inconsistent");
            if (found != overrides.end() && found->second->profile_overrides)
                profiles = &*found->second->profile_overrides;
        }
        require(profiles->size() <= 4096, "directional resize own profile collection budget exceeded");
        own_sources.emplace(node.part_path, profiles);
    }
    std::map<Path, std::size_t> source_counts;
    for (const auto& profile : actual.profiles) {
        const auto& profiles = *own_sources.at(profile.part_path);
        auto& count = source_counts[profile.part_path];
        require(count < profiles.size() && profiles[count] == profile.profile,
            "directional resize own profile source does not match the actual model");
        ++count;
    }
    for (const auto& [path, profiles] : own_sources)
        require(source_counts[path] == profiles->size(), "directional resize own profile source is incomplete");
    const auto axes = assembly_rotation_components(operation.world_axis_rotation_radians.value_or(
        source.root->transform.rotation_radians));
    const bool unit = operation.x_factor == 1.0 && operation.y_factor == 1.0;
    auto pending = available_work;
    if (!unit) charge_linear(pending, source.segments);
    std::optional<ExactOperationFrame> frame;
    if (operation.operation_frame) {
        charge_frame_coefficients(pending, unit ? 0 : actual.profiles.size());
        const auto [fc, fs] = assembly_rotation_components(operation.operation_frame->rotation_radians);
        const Exact cosine = binary64(fc), sine = binary64(fs);
        // Form F's basis times the explicit local axis basis exactly. Actual
        // binary64 norms are retained by resized_profile's conjugation.
        frame = ExactOperationFrame{cosine, sine,
            cosine * binary64(axes.cosine) - sine * binary64(axes.sine),
            sine * binary64(axes.cosine) + cosine * binary64(axes.sine)};
        require(frame->axis_cosine * frame->axis_cosine + frame->axis_sine * frame->axis_sine > 0,
            "directional resize operation frame axis basis is singular");
    }
    AssemblyDirectionalResizeDraft result; result.source_instance = actual.source_instance;
    std::map<Path, std::size_t> destinations;
    for (const auto& node : actual.nodes) {
        AssemblyDirectionalResizeNodeDraft draft;
        draft.part_path = node.part_path; draft.type_id = node.type_id;
        draft.actual_world_transform = node.transform;
        if (node.part_path.empty()) draft.source_profile_overrides = actual.source_instance.profile_overrides;
        else if (node.path_override) draft.source_profile_overrides = node.path_override->profile_overrides;
        destinations.emplace(node.part_path, result.nodes.size());
        result.nodes.push_back(std::move(draft));
    }
    for (const auto& profile : actual.profiles) {
        auto& node = result.nodes.at(destinations.at(profile.part_path));
        node.source_profiles.push_back(profile.profile);
        node.candidate_profiles.push_back(unit ? profile.profile :
            resized_profile(profile.profile, profile.transform, axes, operation, frame ? &*frame : nullptr));
    }
    if (unit) {
        if (frame) available_work = std::move(pending);
        return result;
    }
    auto candidate = actual.source_instance;
    for (const auto& node : result.nodes) {
        if (node.source_profiles == node.candidate_profiles) continue;
        result.changed = true;
        if (node.part_path.empty()) candidate.profile_overrides = node.candidate_profiles;
        else {
            auto found = std::find_if(candidate.nested_overrides.begin(), candidate.nested_overrides.end(),
                [&](const auto& row) { return row.part_path == node.part_path; });
            if (found == candidate.nested_overrides.end()) {
                AssemblyPathOverride row; row.part_path = node.part_path;
                candidate.nested_overrides.push_back(std::move(row));
                found = std::prev(candidate.nested_overrides.end());
            }
            found->profile_overrides = node.candidate_profiles;
        }
    }
    // Full normal node-local/material/physical/world/volume admission. This
    // detached expansion is validation only; no intent or model is published.
    const auto admitted = model.expand(candidate, pending);
    require(admitted.nodes.size() == actual.nodes.size() && admitted.profiles.size() == actual.profiles.size(),
        "directional resize source expansion does not match the actual model");
    for (const auto& node : admitted.nodes) {
        const auto found = source.nodes.find(node.part_path);
        require(found != source.nodes.end() && found->second->type_id == node.type_id &&
            found->second->transform == node.transform,
            "directional resize changed an actual node transform/definition");
    }
    std::map<Path, std::size_t> admitted_counts;
    for (const auto& profile : admitted.profiles) {
        const auto& profiles = result.nodes.at(destinations.at(profile.part_path)).candidate_profiles;
        auto& count = admitted_counts[profile.part_path];
        require(count < profiles.size() && profiles[count] == profile.profile,
            "directional resize candidate own profiles are inconsistent");
        ++count;
    }
    for (const auto& node : result.nodes) {
        require(admitted_counts[node.part_path] == node.candidate_profiles.size(),
            "directional resize candidate own profiles are incomplete");
    }
    available_work = std::move(pending);
    return result;
}

Bounds2 assembly_root_oriented_profile_bounds(const AssemblyExpansion& actual, Vec2 world_origin_m,
    AssemblyExpansionBudget& available_work) {
    require(std::isfinite(world_origin_m.x) && std::isfinite(world_origin_m.y),
        "assembly bounds origin must be finite");
    const auto source = index_source(actual, available_work);
    return profile_bounds(actual, source, world_origin_m,
        assembly_rotation_components(source.root->transform.rotation_radians),
        source.root->transform.mirrored_y, available_work);
}

Bounds2 assembly_world_axis_profile_bounds(const AssemblyExpansion& actual, Vec2 world_origin_m,
    double world_axis_rotation_radians, AssemblyExpansionBudget& available_work) {
    require(std::isfinite(world_origin_m.x) && std::isfinite(world_origin_m.y),
        "assembly bounds origin must be finite");
    require(std::isfinite(world_axis_rotation_radians), "assembly bounds world axis rotation must be finite");
    const auto source = index_source(actual, available_work);
    return profile_bounds(actual, source, world_origin_m,
        assembly_rotation_components(world_axis_rotation_radians), false, available_work);
}

} // namespace sketch
