#include "sketch/assembly_geometry.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_profile_geometry.hpp"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_XYZ.hxx>
#include <cmath>
#include <stdexcept>
#include <string>

namespace sketch {
namespace {

TopoDS_Shape make_assembly_profile_shape(const AssemblyProfile& profile) {
    const auto source_shape = [&](const Boundary& outer, const std::vector<Boundary>& holes) {
        return make_slab(Slab{profile.id, outer, holes, profile.height_m, profile.elevation_m});
    };
    if (!profile.planar_affine) return source_shape(profile.outer, profile.holes);
    const auto& map = *profile.planar_affine;
    validate_assembly_profile_affine(map);
    if (map.xx == 1 && map.xy == 0 && map.yx == 0 && map.yy == 1 &&
        map.source_origin.x == map.target_origin.x && map.source_origin.y == map.target_origin.y)
        return source_shape(profile.outer, profile.holes);
    if (profile.outer.empty()) throw std::invalid_argument("Mapped assembly profile has no outer ring");

    // Construct near an actual source vertex, not at the authored map origin.
    // Computing target_origin - M*source_origin can overflow or discard the
    // small residual even when every effective profile point is representable.
    const auto anchor = profile.outer.front().start;
    const auto mapped_anchor = assembly_profile_affine_point(anchor, map);
    const AssemblyProfilePlanarAffine shift{anchor, {0, 0}};
    const gp_Mat linear(map.xx, map.xy, 0, map.yx, map.yy, 0, 0, 0, 1);
    const gp_GTrsf affine(linear, gp_XYZ(mapped_anchor.x, mapped_anchor.y, 0));
    const auto native_determinant = linear.Determinant();
    if (!std::isfinite(native_determinant) || native_determinant <= 0 || affine.IsSingular())
        throw std::invalid_argument("Assembly profile map is not representable by the native kernel");
    const auto inverse = linear.Inverted();
    for (int row = 1; row <= 3; ++row)
        for (int column = 1; column <= 3; ++column)
            if (!std::isfinite(inverse.Value(row, column)))
                throw std::invalid_argument("Assembly profile native inverse is not finite");
    const auto native_point = [&](Vec2 point) {
        gp_XYZ coordinates(point.x, point.y, 0);
        affine.Transforms(coordinates);
        if (!std::isfinite(coordinates.X()) || !std::isfinite(coordinates.Y()))
            throw std::invalid_argument("Mapped assembly native profile coordinates are not finite");
        return Vec2{coordinates.X(), coordinates.Y()};
    };
    const auto shift_ring = [&](const Boundary& ring) {
        // Check the exact effective curve range independently of native kernel
        // arithmetic; a finite endpoint alone does not bound an elliptical arc.
        (void)assembly_profile_boundary_bounds(ring, map);
        auto shifted = ring;
        for (auto& segment : shifted) {
            const auto start = assembly_profile_affine_point(segment.start, map);
            const auto end = assembly_profile_affine_point(segment.end, map);
            if (std::hypot(end.x - start.x, end.y - start.y) <= default_geometry_tolerance_metres)
                throw std::invalid_argument("Mapped assembly native profile edge collapses below tolerance");
            segment.start = assembly_profile_affine_point(segment.start, shift);
            segment.end = assembly_profile_affine_point(segment.end, shift);
            const auto native_start = native_point(segment.start);
            const auto native_end = native_point(segment.end);
            const auto native_chord = std::hypot(native_end.x - native_start.x, native_end.y - native_start.y);
            if (!std::isfinite(native_chord) || native_chord <= default_geometry_tolerance_metres)
                throw std::invalid_argument("Mapped assembly native profile edge is not representable");
        }
        return shifted;
    };
    auto outer = shift_ring(profile.outer);
    std::vector<Boundary> holes;
    holes.reserve(profile.holes.size());
    for (const auto& hole : profile.holes) holes.push_back(shift_ring(hole));
    const auto local = source_shape(outer, holes);

    // A genuine native affinity transforms the supporting curves/surfaces.
    // Circular source arcs become their exact affine images, never new circles
    // assembled from stretched endpoints and an unchanged sweep. Z is untouched.
    BRepBuilderAPI_GTransform transformed(local, affine, true);
    if (!transformed.IsDone() || transformed.Shape().IsNull() ||
        !BRepCheck_Analyzer(transformed.Shape()).IsValid())
        throw std::invalid_argument("Assembly profile map did not produce a valid native shape");
    const auto result = transformed.Shape();
    const auto volume = solid_volume(result);
    if (!std::isfinite(volume) || volume <= 0)
        throw std::invalid_argument("Mapped assembly native body volume must be finite and positive");
    return result;
}

} // namespace

TopoDS_Shape transform_assembly_shape(const TopoDS_Shape& source,const AssemblyTransform& placement) {
    const auto& offset=placement.translation_m;
    const auto scale_z=placement.scale*placement.vertical_scale;
    if(!std::isfinite(placement.scale) || placement.scale<=0 ||
        !std::isfinite(placement.vertical_scale) || placement.vertical_scale<=0 ||
        !std::isfinite(scale_z) || scale_z<=0 || !std::isfinite(placement.rotation_radians) ||
        !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
        throw std::invalid_argument("Assembly solid transform must be finite with positive XY and Z scales");
    }
    try {
        if(source.IsNull() || !BRepCheck_Analyzer(source).IsValid())
            throw std::invalid_argument("Assembly solid transform requires a valid source shape");
        const auto [c,s]=assembly_rotation_components(placement.rotation_radians);
        TopoDS_Shape result;
        if(placement.vertical_scale==1) {
            gp_Trsf mirror, scale, rotation, translation;
            if(placement.mirrored_y)
                mirror.SetMirror(gp_Ax2(gp_Pnt(0,0,0),gp_Dir(0,1,0)));
            scale.SetScale(gp_Pnt(0,0,0),placement.scale);
            rotation.SetValues(c,-s,0,0,s,c,0,0,0,0,1,0);
            translation.SetTranslation(gp_Vec(offset.x,offset.y,offset.z));
            // OCCT multiplication applies the rightmost transform first.
            BRepBuilderAPI_Transform transformed(source,translation*rotation*scale*mirror,true);
            if(!transformed.IsDone())throw std::invalid_argument("Assembly solid transform failed");
            result=transformed.Shape();
        } else {
            const auto parity=placement.mirrored_y ? -1.0:1.0;
            const gp_Mat linear(placement.scale*c,-placement.scale*s*parity,0,
                placement.scale*s,placement.scale*c*parity,0,0,0,scale_z);
            const gp_GTrsf affine(linear,gp_XYZ(offset.x,offset.y,offset.z));
            BRepBuilderAPI_GTransform transformed(source,affine,true);
            if(!transformed.IsDone())throw std::invalid_argument("Assembly affine solid transform failed");
            result=transformed.Shape();
        }
        if(result.IsNull() || !BRepCheck_Analyzer(result).IsValid())
            throw std::invalid_argument("Assembly transform did not produce a valid native shape");
        const auto volume=solid_volume(result);
        if(!std::isfinite(volume) || volume<=0)
            throw std::invalid_argument("Assembly transformed body volume must be finite and positive");
        return result;
    } catch(const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Assembly solid transform failed: ")+error.what());
    }
}

AssemblyGeometry make_assembly_geometry(const AssemblyExpansion& expansion) {
    AssemblyGeometry result;
    result.solids.reserve(expansion.profiles.size());
    if (expansion.profiles.empty()) return result;
    try {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const auto& source : expansion.profiles) {
            const auto& profile = source.profile;
            const auto local = make_assembly_profile_shape(profile);
            const auto transformed=transform_assembly_shape(local,source.transform);
            const double volume = solid_volume(transformed);
            if (!std::isfinite(volume) || volume <= 0) {
                throw std::invalid_argument("Assembly solid volume must be finite and positive");
            }
            result.volume_m3 += volume;
            if (!std::isfinite(result.volume_m3)) {
                throw std::invalid_argument("Assembly solid volume exceeds the supported numeric range");
            }
            result.solids.push_back({source, transformed, volume});
            builder.Add(compound, result.solids.back().shape);
        }
        if (!BRepCheck_Analyzer(compound).IsValid()) {
            throw std::invalid_argument("Assembly solids did not produce a valid compound");
        }
        result.shape = compound;
        return result;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Assembly geometry failed: ") + error.what());
    }
}

} // namespace sketch
